#include "VulkanTestDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// s_mov_b32 s8, s5; v_mov_b32 v1, 0; loop: v_add_nc_u32 v1, 1, v1; s_sub_u32 s8, s8, 1;
// s_cmp_lg_u32 s8, 0; s_cbranch_scc1 loop (keeps queue A's work on the GPU while queue B reaches its
// wait); v_mov_b32 v0, s4; buffer_store_dword v0, off, s[0:3]; buffer_store_dword v1, off, s[0:3]
// offset:4; s_endpgm
alignas(256) std::array<std::uint32_t, 12> WriterCode{0xbe880305u, 0x7e020280u, 0x4a020281u, 0x80888108u, 0xbf078008u, 0xbf85fffcu, 0x7e000204u, 0xe0700000u, 0x80000000u, 0xe0700004u, 0x80000100u, 0xbf810000u};
// buffer_load_dword v0, off, s[0:3]; s_waitcnt vmcnt(0); buffer_store_dword v0, off, s[4:7]; s_endpgm
alignas(256) std::array<std::uint32_t, 6> CopyCode{0xe0300000u, 0x80000000u, 0xbf8c3f70u, 0xe0700000u, 0x80010000u, 0xbf810000u};

// Guest memory (host-imported, so labels are stored on the GPU in the recorded stream), one slot
// per round.
constexpr std::uint32_t Rounds = 4u;
struct alignas(256) Slot {
    alignas(256) std::array<std::uint32_t, 4> produced{};
    alignas(256) std::array<std::uint32_t, 4> consumed{};
    alignas(256) volatile std::uint32_t label = 0;
    alignas(256) volatile std::uint32_t done = 0;
};
struct alignas(4096) GuestData {
    std::array<Slot, Rounds> slots{};
};
GuestData guest;

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Program(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
}

void Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    Program(words, code, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void WriteLabel(std::vector<std::uint32_t>& words, const volatile std::uint32_t* label, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(label), High(label), value});
}

}

// Queue A writes a value, then a label; queue B waits on the label (WAIT_REG_MEM) and copies the
// value. A's work is recorded before B reaches the wait, so the wait is ordered on the GPU (the one
// recorder stream): B's worker does not block on the GPU and B's copy still reads A's value.
int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        GuestAllocations::Mutation().Add(&guest, sizeof(guest), true, true, true);
        constexpr std::uint32_t value = 0x5eedf00du;
        for (std::uint32_t round = 0; round < Rounds; ++round) {
            auto& Produced = guest.slots[round].produced;
            auto& Consumed = guest.slots[round].consumed;
            auto& QueueLabel = guest.slots[round].label;
            auto& Done = guest.slots[round].done;
            const auto produced = Descriptor(Produced.data(), sizeof(Produced));
            const auto consumed = Descriptor(Consumed.data(), sizeof(Consumed));
            std::vector<std::uint32_t> producer;
            Dispatch(producer, WriterCode.data(), {produced[0], produced[1], produced[2], produced[3], value + round, 30000000u});
            WriteLabel(producer, &QueueLabel, round + 1u);
            std::vector<std::uint32_t> consumer;
            consumer.insert(consumer.end(), {0xc0053c00u, 0x13u, Low(&QueueLabel), High(&QueueLabel), round + 1u, 0xffffffffu, 0x19u});
            Dispatch(consumer, CopyCode.data(), {produced[0], produced[1], produced[2], produced[3], consumed[0], consumed[1], consumed[2], consumed[3]});
            WriteLabel(consumer, &Done, 1u);
            std::uint64_t orderedBefore = 0, blockedBefore = 0;
            AgcDriverGpuWaitCounts_nid_postfix(&orderedBefore, &blockedBefore);
            Packet first{producer.data(), static_cast<std::uint32_t>(producer.size()), 0, {}};
            Require(sceAgcDriverSubmitAcb(0x20, &first) == 0, "producer submit failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Packet second{consumer.data(), static_cast<std::uint32_t>(consumer.size()), 0, {}};
            Require(sceAgcDriverSubmitAcb(0x21, &second) == 0, "consumer submit failed");
            AgcDriverWaitIdle_nid_postfix();
            // The queues are idle; the last batch's results reach guest memory when it completes.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (Done != 1u && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::uint64_t orderedAfter = 0, blockedAfter = 0;
            AgcDriverGpuWaitCounts_nid_postfix(&orderedAfter, &blockedAfter);
            std::printf("round %u: waits ordered on the GPU %llu, blocked %llu\n", round, static_cast<unsigned long long>(orderedAfter - orderedBefore), static_cast<unsigned long long>(blockedAfter - blockedBefore));
            Require(Done == 1u && QueueLabel == round + 1u, "a queue did not finish (done " + std::to_string(Done) + ", label " + std::to_string(QueueLabel) + ", produced " + std::to_string(Produced[0]) + ", consumed " + std::to_string(Consumed[0]) + ")");
            Require(Produced[0] == value + round, "queue A did not write its value");
            Require(Consumed[0] == value + round, "queue B's copy behind the wait read " + std::to_string(Consumed[0]) + " instead of queue A's value");
            // Round 0 compiles the shaders: queue A may not have recorded its label when B waits.
            if (round != 0u) Require(blockedAfter == blockedBefore && orderedAfter > orderedBefore, "queue B's worker blocked on the memory wait although queue A's label was recorded");
        }
        std::cout << "cross-queue GPU-ordered wait test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
