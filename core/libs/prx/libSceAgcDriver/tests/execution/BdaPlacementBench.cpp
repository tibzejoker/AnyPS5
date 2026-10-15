#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "VulkanTestDevice.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Check;
using AgcDriver::Graphics::Require;

// The kernel (GLSL, glslangValidator -V --target-env vulkan1.2): 64-lane groups read 16-byte nodes
// through a buffer reference (BDA, as the recompiled programs do) and store one dword per lane.
// Mode 0 walks a dependent chain (each node names the next: a BVH traversal), mode 1 streams.
constexpr std::uint32_t PlacementSpirv[] = {
	0x07230203,0x00010500,0x0008000b,0x00000079,0x00000000,0x00020011,0x00000001,0x00020011,
	0x0000000b,0x00020011,0x000014e3,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
	0x00000000,0x0003000e,0x000014e4,0x00000001,0x0007000f,0x00000005,0x00000004,0x6e69616d,
	0x00000000,0x0000000b,0x00000019,0x00060010,0x00000004,0x00000011,0x00000040,0x00000001,
	0x00000001,0x00030003,0x00000002,0x000001cc,0x00070004,0x455f4c47,0x625f5458,0x65666675,
	0x65725f72,0x65726566,0x0065636e,0x000d0004,0x455f4c47,0x735f5458,0x65646168,0x78655f72,
	0x63696c70,0x615f7469,0x68746972,0x6974656d,0x79745f63,0x5f736570,0x36746e69,0x00000034,
	0x00040005,0x00000004,0x6e69616d,0x00000000,0x00040005,0x00000008,0x656e616c,0x00000000,
	0x00080005,0x0000000b,0x475f6c67,0x61626f6c,0x766e496c,0x7461636f,0x496e6f69,0x00000044,
	0x00040005,0x00000013,0x65646f4e,0x00000073,0x00050006,0x00000013,0x00000000,0x65646f6e,
	0x00000000,0x00040005,0x00000015,0x65646f6e,0x00000073,0x00040005,0x00000017,0x68737550,
	0x00000000,0x00050006,0x00000017,0x00000000,0x65646f6e,0x00000073,0x00050006,0x00000017,
	0x00000001,0x7374756f,0x00000000,0x00050006,0x00000017,0x00000002,0x6e756f63,0x00000074,
	0x00050006,0x00000017,0x00000003,0x70657473,0x00000073,0x00050006,0x00000017,0x00000004,
	0x65646f6d,0x00000000,0x00030005,0x00000019,0x00000070,0x00030005,0x00000020,0x006d7573,
	0x00040005,0x00000029,0x65646e69,0x00000078,0x00040005,0x00000031,0x70657473,0x00000000,
	0x00040005,0x0000003d,0x65646f6e,0x00000000,0x00040005,0x0000004e,0x70657473,0x00000000,
	0x00040005,0x00000058,0x65646f6e,0x00000000,0x00030005,0x00000071,0x0074754f,0x00050006,
	0x00000071,0x00000000,0x756c6176,0x00000065,0x00040047,0x0000000b,0x0000000b,0x0000001c,
	0x00040047,0x00000012,0x00000006,0x00000010,0x00030047,0x00000013,0x00000002,0x00050048,
	0x00000013,0x00000000,0x00000023,0x00000000,0x00030047,0x00000015,0x000014ec,0x00030047,
	0x00000017,0x00000002,0x00050048,0x00000017,0x00000000,0x00000023,0x00000000,0x00050048,
	0x00000017,0x00000001,0x00000023,0x00000008,0x00050048,0x00000017,0x00000002,0x00000023,
	0x00000010,0x00050048,0x00000017,0x00000003,0x00000023,0x00000014,0x00050048,0x00000017,
	0x00000004,0x00000023,0x00000018,0x00040047,0x00000070,0x00000006,0x00000004,0x00030047,
	0x00000071,0x00000002,0x00050048,0x00000071,0x00000000,0x00000023,0x00000000,0x00040047,
	0x00000078,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
	0x00040015,0x00000006,0x00000020,0x00000000,0x00040020,0x00000007,0x00000007,0x00000006,
	0x00040017,0x00000009,0x00000006,0x00000003,0x00040020,0x0000000a,0x00000001,0x00000009,
	0x0004003b,0x0000000a,0x0000000b,0x00000001,0x0004002b,0x00000006,0x0000000c,0x00000000,
	0x00040020,0x0000000d,0x00000001,0x00000006,0x00030027,0x00000010,0x000014e5,0x00040017,
	0x00000011,0x00000006,0x00000004,0x0003001d,0x00000012,0x00000011,0x0003001e,0x00000013,
	0x00000012,0x00040020,0x00000010,0x000014e5,0x00000013,0x00040020,0x00000014,0x00000007,
	0x00000010,0x00040015,0x00000016,0x00000040,0x00000000,0x0007001e,0x00000017,0x00000016,
	0x00000016,0x00000006,0x00000006,0x00000006,0x00040020,0x00000018,0x00000009,0x00000017,
	0x0004003b,0x00000018,0x00000019,0x00000009,0x00040015,0x0000001a,0x00000020,0x00000001,
	0x0004002b,0x0000001a,0x0000001b,0x00000000,0x00040020,0x0000001c,0x00000009,0x00000016,
	0x0004002b,0x0000001a,0x00000021,0x00000004,0x00040020,0x00000022,0x00000009,0x00000006,
	0x00020014,0x00000025,0x0004002b,0x00000006,0x0000002b,0x9e3779b1,0x0004002b,0x0000001a,
	0x0000002d,0x00000002,0x0004002b,0x0000001a,0x00000038,0x00000003,0x00040020,0x0000003c,
	0x00000007,0x00000011,0x00040020,0x00000040,0x000014e5,0x00000011,0x0004002b,0x00000006,
	0x00000043,0x00000001,0x0004002b,0x0000001a,0x0000004b,0x00000001,0x0004002b,0x00000006,
	0x0000005c,0x00001000,0x00030027,0x0000006f,0x000014e5,0x0003001d,0x00000070,0x00000006,
	0x0003001e,0x00000071,0x00000070,0x00040020,0x0000006f,0x000014e5,0x00000071,0x00040020,
	0x00000075,0x000014e5,0x00000006,0x0004002b,0x00000006,0x00000077,0x00000040,0x0006002c,
	0x00000009,0x00000078,0x00000077,0x00000043,0x00000043,0x00050036,0x00000002,0x00000004,
	0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000007,0x00000008,0x00000007,
	0x0004003b,0x00000014,0x00000015,0x00000007,0x0004003b,0x00000007,0x00000020,0x00000007,
	0x0004003b,0x00000007,0x00000029,0x00000007,0x0004003b,0x00000007,0x00000031,0x00000007,
	0x0004003b,0x0000003c,0x0000003d,0x00000007,0x0004003b,0x00000007,0x0000004e,0x00000007,
	0x0004003b,0x0000003c,0x00000058,0x00000007,0x00050041,0x0000000d,0x0000000e,0x0000000b,
	0x0000000c,0x0004003d,0x00000006,0x0000000f,0x0000000e,0x0003003e,0x00000008,0x0000000f,
	0x00050041,0x0000001c,0x0000001d,0x00000019,0x0000001b,0x0004003d,0x00000016,0x0000001e,
	0x0000001d,0x00040078,0x00000010,0x0000001f,0x0000001e,0x0003003e,0x00000015,0x0000001f,
	0x0003003e,0x00000020,0x0000000c,0x00050041,0x00000022,0x00000023,0x00000019,0x00000021,
	0x0004003d,0x00000006,0x00000024,0x00000023,0x000500aa,0x00000025,0x00000026,0x00000024,
	0x0000000c,0x000300f7,0x00000028,0x00000000,0x000400fa,0x00000026,0x00000027,0x0000004d,
	0x000200f8,0x00000027,0x0004003d,0x00000006,0x0000002a,0x00000008,0x00050084,0x00000006,
	0x0000002c,0x0000002a,0x0000002b,0x00050041,0x00000022,0x0000002e,0x00000019,0x0000002d,
	0x0004003d,0x00000006,0x0000002f,0x0000002e,0x00050089,0x00000006,0x00000030,0x0000002c,
	0x0000002f,0x0003003e,0x00000029,0x00000030,0x0003003e,0x00000031,0x0000000c,0x000200f9,
	0x00000032,0x000200f8,0x00000032,0x000400f6,0x00000034,0x00000035,0x00000000,0x000200f9,
	0x00000036,0x000200f8,0x00000036,0x0004003d,0x00000006,0x00000037,0x00000031,0x00050041,
	0x00000022,0x00000039,0x00000019,0x00000038,0x0004003d,0x00000006,0x0000003a,0x00000039,
	0x000500b0,0x00000025,0x0000003b,0x00000037,0x0000003a,0x000400fa,0x0000003b,0x00000033,
	0x00000034,0x000200f8,0x00000033,0x0004003d,0x00000010,0x0000003e,0x00000015,0x0004003d,
	0x00000006,0x0000003f,0x00000029,0x00060041,0x00000040,0x00000041,0x0000003e,0x0000001b,
	0x0000003f,0x0006003d,0x00000011,0x00000042,0x00000041,0x00000002,0x00000010,0x0003003e,
	0x0000003d,0x00000042,0x00050041,0x00000007,0x00000044,0x0000003d,0x00000043,0x0004003d,
	0x00000006,0x00000045,0x00000044,0x0004003d,0x00000006,0x00000046,0x00000020,0x00050080,
	0x00000006,0x00000047,0x00000046,0x00000045,0x0003003e,0x00000020,0x00000047,0x00050041,
	0x00000007,0x00000048,0x0000003d,0x0000000c,0x0004003d,0x00000006,0x00000049,0x00000048,
	0x0003003e,0x00000029,0x00000049,0x000200f9,0x00000035,0x000200f8,0x00000035,0x0004003d,
	0x00000006,0x0000004a,0x00000031,0x00050080,0x00000006,0x0000004c,0x0000004a,0x0000004b,
	0x0003003e,0x00000031,0x0000004c,0x000200f9,0x00000032,0x000200f8,0x00000034,0x000200f9,
	0x00000028,0x000200f8,0x0000004d,0x0003003e,0x0000004e,0x0000000c,0x000200f9,0x0000004f,
	0x000200f8,0x0000004f,0x000400f6,0x00000051,0x00000052,0x00000000,0x000200f9,0x00000053,
	0x000200f8,0x00000053,0x0004003d,0x00000006,0x00000054,0x0000004e,0x00050041,0x00000022,
	0x00000055,0x00000019,0x00000038,0x0004003d,0x00000006,0x00000056,0x00000055,0x000500b0,
	0x00000025,0x00000057,0x00000054,0x00000056,0x000400fa,0x00000057,0x00000050,0x00000051,
	0x000200f8,0x00000050,0x0004003d,0x00000010,0x00000059,0x00000015,0x0004003d,0x00000006,
	0x0000005a,0x00000008,0x0004003d,0x00000006,0x0000005b,0x0000004e,0x00050084,0x00000006,
	0x0000005d,0x0000005b,0x0000005c,0x00050080,0x00000006,0x0000005e,0x0000005a,0x0000005d,
	0x00050041,0x00000022,0x0000005f,0x00000019,0x0000002d,0x0004003d,0x00000006,0x00000060,
	0x0000005f,0x00050089,0x00000006,0x00000061,0x0000005e,0x00000060,0x00060041,0x00000040,
	0x00000062,0x00000059,0x0000001b,0x00000061,0x0006003d,0x00000011,0x00000063,0x00000062,
	0x00000002,0x00000010,0x0003003e,0x00000058,0x00000063,0x00050041,0x00000007,0x00000064,
	0x00000058,0x0000000c,0x0004003d,0x00000006,0x00000065,0x00000064,0x00050041,0x00000007,
	0x00000066,0x00000058,0x00000043,0x0004003d,0x00000006,0x00000067,0x00000066,0x000500c6,
	0x00000006,0x00000068,0x00000065,0x00000067,0x0004003d,0x00000006,0x00000069,0x00000020,
	0x00050080,0x00000006,0x0000006a,0x00000069,0x00000068,0x0003003e,0x00000020,0x0000006a,
	0x000200f9,0x00000052,0x000200f8,0x00000052,0x0004003d,0x00000006,0x0000006b,0x0000004e,
	0x00050080,0x00000006,0x0000006c,0x0000006b,0x0000004b,0x0003003e,0x0000004e,0x0000006c,
	0x000200f9,0x0000004f,0x000200f8,0x00000051,0x000200f9,0x00000028,0x000200f8,0x00000028,
	0x00050041,0x0000001c,0x0000006d,0x00000019,0x0000004b,0x0004003d,0x00000016,0x0000006e,
	0x0000006d,0x00040078,0x0000006f,0x00000072,0x0000006e,0x0004003d,0x00000006,0x00000073,
	0x00000008,0x0004003d,0x00000006,0x00000074,0x00000020,0x00060041,0x00000075,0x00000076,
	0x00000072,0x0000001b,0x00000073,0x0005003e,0x00000076,0x00000074,0x00000002,0x00000004,
	0x000100fd,0x00010038
};

constexpr std::uint32_t NodeCount = 1u << 22u;
constexpr std::size_t NodeBytes = static_cast<std::size_t>(NodeCount) * 16u;
constexpr std::uint32_t Lanes = 1u << 20u;
constexpr std::uint32_t Rounds = 5u;

struct Push {
    std::uint64_t nodes;
    std::uint64_t outs;
    std::uint32_t count;
    std::uint32_t steps;
    std::uint32_t mode;
};

std::uint32_t memoryType(const AgcDriver::Graphics::Context& context, std::uint32_t bits, VkMemoryPropertyFlags wanted) {
    for (std::uint32_t index = 0; index < context.memory.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0u && (context.memory.memoryTypes[index].propertyFlags & wanted) == wanted) return index;
    }
    return ~0u;
}

// The nodes in guest-like host memory (VirtualAlloc), imported with VK_EXT_external_memory_host as
// GuestBufferMemory imports guest ranges.
struct HostImported {
    const AgcDriver::Graphics::Context& context;
    void* host = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    explicit HostImported(const AgcDriver::Graphics::Context& context) : context(context) {
        host = VirtualAlloc(nullptr, NodeBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Require(host != nullptr, "placement bench: VirtualAlloc failed");
        VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        Check(context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &pointer), "vkGetMemoryHostPointerPropertiesEXT");
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        info.size = NodeBytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer import");
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocate.allocationSize = NodeBytes;
        allocate.memoryTypeIndex = memoryType(context, pointer.memoryTypeBits, 0u);
        Require(allocate.memoryTypeIndex != ~0u, "placement bench: no memory type for the host import");
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocate, nullptr, &memory), "vkAllocateMemory import");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory import");
        const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
        address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    }
    ~HostImported() {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        VirtualFree(host, 0, MEM_RELEASE);
    }
};

double TimeDispatch(const AgcDriver::Graphics::Context& context, VkPipeline pipeline, VkPipelineLayout layout, const Push& push) {
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    VkQueryPool queries = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateQueryPool>("vkCreateQueryPool")(context.device, &queryInfo, nullptr, &queries), "vkCreateQueryPool");
    std::vector<double> times;
    for (std::uint32_t round = 0; round < Rounds; ++round) {
        AgcDriver::Graphics::CommandBatch batch(context);
        const auto commands = batch.Handle();
        context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, queries, 0, 2);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, Lanes / 64u, 1, 1);
        context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        batch.SubmitAndWait();
        std::array<std::uint64_t, 2> stamps{};
        Check(context.Function<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(context.device, queries, 0, 2, sizeof(stamps), stamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        times.push_back(static_cast<double>(stamps[1] - stamps[0]) * context.limits.timestampPeriod / 1e6);
    }
    context.Function<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(context.device, queries, nullptr);
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// GPU time of the same BDA reads from guest memory imported from the host (what GuestBufferMemory
// maps today: system memory, reached over PCIe on a discrete GPU) and from a device-local copy.
void Run(AgcDriver::VulkanDevice& device) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    HostImported imported(context);
    auto* nodes = static_cast<std::uint32_t*>(imported.host);
    std::uint32_t state = 12345u;
    for (std::uint32_t index = 0; index < NodeCount; ++index) {
        state = state * 1664525u + 1013904223u;
        nodes[index * 4u] = state % NodeCount;
        nodes[index * 4u + 1u] = index;
        nodes[index * 4u + 2u] = 0u;
        nodes[index * 4u + 3u] = 0u;
    }
    AgcDriver::Graphics::Buffer local(context, NodeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    {
        AgcDriver::Graphics::CommandBatch batch(context);
        const VkBufferCopy region{0, 0, NodeBytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(batch.Handle(), imported.buffer, local.Handle(), 1, &region);
        batch.SubmitAndWait();
    }
    AgcDriver::Graphics::Buffer outputs(context, static_cast<std::size_t>(Lanes) * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(PlacementSpirv);
    moduleInfo.pCode = PlacementSpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
    std::uint32_t deviceLocalHeaps = 0;
    for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount; ++heap) deviceLocalHeaps += (context.memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u ? 1u : 0u;
    std::printf("memory heaps: %u, device-local %u\n", context.memory.memoryHeapCount, deviceLocalHeaps);
    for (const std::uint32_t mode : {0u, 1u}) {
        const std::uint32_t steps = mode == 0u ? 32u : 64u;
        const auto host = TimeDispatch(context, pipeline, layout, {imported.address, outputs.DeviceAddress(), NodeCount, steps, mode});
        const auto vram = TimeDispatch(context, pipeline, layout, {local.DeviceAddress(), outputs.DeviceAddress(), NodeCount, steps, mode});
        std::printf("%s, %u lanes x %u node reads of 16 bytes (64 MiB of nodes): host-imported %.3f ms, device-local %.3f ms (x%.1f)\n", mode == 0u ? "dependent chain" : "streaming", Lanes, steps, host, vram, vram > 0.0 ? host / vram : 0.0);
    }
    context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::puts("bda placement benchmark passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
