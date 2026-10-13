#include <codegen/x86/RedZoneFramer.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Base = 0x1000;

std::vector<std::uint8_t> padTo(std::vector<std::uint8_t> bytes, const std::size_t size) {
    bytes.resize(size, 0xcc);
    return bytes;
}

void append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
}

std::vector<std::uint8_t> callTo(const std::size_t from, const std::size_t target) {
    const auto displacement = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(from + 5);
    return {0xe8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8), static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)};
}

const std::vector<std::uint8_t> Prologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x53};
const std::vector<std::uint8_t> Exit{0x5b, 0x41, 0x5f, 0x5d, 0xc3};
const std::vector<std::uint8_t> StoreBelowPushes{0x48, 0x89, 0x7d, 0xe8};
const std::vector<std::uint8_t> LoadBelowPushes{0x48, 0x8b, 0x45, 0xe8};
const std::vector<std::uint8_t> LoadAbovePushes{0x48, 0x8b, 0x45, 0x08};

std::vector<std::uint8_t> leaf(const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> bytes = Prologue;
    append(bytes, body);
    append(bytes, Exit);
    return bytes;
}

struct Image {
    std::vector<std::uint8_t> bytes;
    std::vector<Domain::ProgramHeader> segments;
};

Image image(const std::vector<std::vector<std::uint8_t>>& functions) {
    Image result;
    std::vector<std::size_t> starts;
    std::size_t at = 0x40;
    for (const auto& function : functions) {
        starts.push_back(at);
        at += (function.size() + 15) / 16 * 16 + 16;
    }
    std::vector<std::uint8_t> caller;
    for (const auto start : starts) append(caller, callTo(caller.size(), start));
    caller.push_back(0xc3);
    result.bytes = padTo(caller, 0x40);
    for (std::size_t index = 0; index < functions.size(); ++index) {
        result.bytes = padTo(result.bytes, starts[index]);
        append(result.bytes, functions[index]);
    }
    result.bytes = padTo(result.bytes, (result.bytes.size() + 15) / 16 * 16 + 16);
    result.segments.push_back({1, 5, 0, Base, Base, result.bytes.size(), result.bytes.size(), 16});
    return result;
}

void framesRedZoneLeaf() {
    std::vector<std::uint8_t> body = StoreBelowPushes;
    append(body, LoadBelowPushes);
    const auto input = image({leaf(body)});
    Codegen::RedZoneFrameReport report;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, {}, report);
    require(report.RedZoneFunctions == 1 && report.Framed == 1 && report.Exits == 1, "a red-zone leaf was not framed");
    require(sites.size() == 2, "a framed leaf needs one prologue and one exit site");

    const auto& entry = sites[0];
    require(entry.Address == Base + 0x40 && entry.Length == Prologue.size(), "the prologue site does not cover the pushes");
    require(entry.OriginalBytes == Prologue, "the prologue site lost its bytes");
    std::vector<std::uint8_t> entryBody = Prologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0x80, 0xe9, 0, 0, 0, 0});
    require(entry.Body == entryBody && entry.ReturnBranchOffset == Prologue.size() + 5, "the prologue stub does not lower rsp after the pushes");

    const auto& exit = sites[1];
    require(exit.Address == Base + 0x40 + Prologue.size() + body.size() && exit.Length == Exit.size(), "the exit site does not cover the pops and ret");
    std::vector<std::uint8_t> exitBody{0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0};
    append(exitBody, Exit);
    append(exitBody, {0xe9, 0, 0, 0, 0});
    require(exit.Body == exitBody && exit.ReturnBranchOffset == 8 + Exit.size(), "the exit stub does not raise rsp before the pops");
}

void leavesOtherFunctions() {
    std::vector<std::uint8_t> nonLeaf = Prologue;
    append(nonLeaf, StoreBelowPushes);
    append(nonLeaf, {0xff, 0xd0});
    append(nonLeaf, Exit);
    const auto input = image({leaf(LoadAbovePushes), nonLeaf});
    Codegen::RedZoneFrameReport report;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, {}, report);
    require(sites.empty() && report.RedZoneFunctions == 0, "a leaf without red-zone use or a function that calls was framed");
}

void skipsSitesTakenByOtherStubs() {
    const auto input = image({leaf(StoreBelowPushes)});
    Codegen::TrampolineSite other{};
    other.Address = Base + 0x40;
    other.Length = 5;
    Codegen::RedZoneFrameReport report;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, {other}, report);
    require(sites.empty() && report.Skipped.at("overlaps another stub") == 1, "a site over another stub was framed");
}

}

int main() {
    try {
        framesRedZoneLeaf();
        leavesOtherFunctions();
        skipsSitesTakenByOtherStubs();
        std::cout << "red-zone framer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "red-zone framer tests failed: " << error.what() << '\n';
        return 1;
    }
}
