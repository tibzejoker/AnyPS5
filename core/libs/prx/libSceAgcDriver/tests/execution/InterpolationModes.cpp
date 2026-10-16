#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::Graphics::Require;

struct Row {
    std::array<std::uint32_t, 9> input;
    std::array<std::array<std::uint32_t, 3>, 8> expected;
};

constexpr std::array<const char*, 8> ModeNames{"IEEE=0 f32-flush f16-flush", "IEEE=0 f32-flush f16-keep", "IEEE=0 f32-keep f16-flush", "IEEE=0 f32-keep f16-keep",
    "IEEE=1 f32-flush f16-flush", "IEEE=1 f32-flush f16-keep", "IEEE=1 f32-keep f16-flush", "IEEE=1 f32-keep f16-keep"};
constexpr std::array<std::uint32_t, 8> ModeFlags{
    InterpolationFlush32 | InterpolationFlush16, InterpolationFlush32, InterpolationFlush16, 0u,
    InterpolationQuiet | InterpolationFlush32 | InterpolationFlush16, InterpolationQuiet | InterpolationFlush32, InterpolationQuiet | InterpolationFlush16, InterpolationQuiet};
constexpr std::array<const char*, 3> OpNames{"v_interp_p1ll_f16", "v_interp_p2_f16", "v_interp_p1lv_f16"};

constexpr std::array<Row, 32> Rows{{
    {{0x33800000u, 0x7fa00000u, 0x43000000u, 0x33800000u, 0x4f000000u, 0xabcd0001u, 0x33800000u, 0x7fa00000u, 0x33800000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00005800u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00005800u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00005800u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00005800u, 0x7fe00000u}}}},
    {{0xbf800000u, 0x00000001u, 0x7fa00000u, 0xbf800000u, 0x477ff000u, 0xabcd8200u, 0xbf800000u, 0x00000001u, 0xb8000000u}, {{{0x7fa00000u, 0x0000fc00u, 0x80000000u}, {0x7fa00000u, 0x0000fc00u, 0xb8000000u}, {0x7fa00000u, 0x0000fc00u, 0x80000001u}, {0x7fa00000u, 0x0000fc00u, 0xb8000000u}, {0x7fe00000u, 0x0000fc00u, 0x80000000u}, {0x7fe00000u, 0x0000fc00u, 0xb8000000u}, {0x7fe00000u, 0x0000fc00u, 0x80000001u}, {0x7fe00000u, 0x0000fc00u, 0xb8000000u}}}},
    {{0x33800000u, 0x7fa00000u, 0xff800000u, 0x33800000u, 0x7f7fffffu, 0xabcdbc00u, 0x33800000u, 0x7fa00000u, 0xbf800000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00007c00u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00007c00u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00007c00u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00007c00u, 0x7fe00000u}}}},
    {{0xb8000000u, 0x7fa00000u, 0x80000000u, 0xb8000000u, 0x7f7fffffu, 0xabcdbc00u, 0xb8000000u, 0x7fa00000u, 0xbf800000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0x33800000u, 0xff800000u, 0xff800000u, 0x33800000u, 0x7f800000u, 0xabcd0000u, 0x33800000u, 0xff800000u, 0x00000000u}, {{{0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xff800000u, 0x00007c00u, 0xff800000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xff800000u, 0x00007c00u, 0xff800000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xff800000u, 0x00007c00u, 0xff800000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xff800000u, 0x00007c00u, 0xff800000u}}}},
    {{0x33800000u, 0x7f800000u, 0xffc02000u, 0x33800000u, 0x47800000u, 0xabcd8000u, 0x33800000u, 0x7f800000u, 0x80000000u}, {{{0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc02000u, 0x00001c00u, 0x7f800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc02000u, 0x00001c00u, 0x7f800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc02000u, 0x00001c00u, 0x7f800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc02000u, 0x00001c00u, 0x7f800000u}}}},
    {{0xb8000000u, 0x477ff000u, 0x00000000u, 0xb8000000u, 0x4f000000u, 0xabcd5800u, 0xb8000000u, 0x477ff000u, 0x43000000u}, {{{0x00000000u, 0x00008000u, 0x43000000u}, {0xbffff000u, 0x0000fc00u, 0x42fc0040u}, {0x00000000u, 0x00008000u, 0x43000000u}, {0xbffff000u, 0x0000fc00u, 0x42fc0040u}, {0x00000000u, 0x00008000u, 0x43000000u}, {0xbffff000u, 0x0000fc00u, 0x42fc0040u}, {0x00000000u, 0x00008000u, 0x43000000u}, {0xbffff000u, 0x0000fc00u, 0x42fc0040u}}}},
    {{0x33800000u, 0x7fa00000u, 0x477fe000u, 0x33800000u, 0x47800000u, 0xabcd7d00u, 0x33800000u, 0x7fa00000u, 0x7fa00000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00001c00u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00001c00u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00001c00u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00001c00u, 0x7fe00000u}}}},
    {{0xb8000000u, 0x47800000u, 0x7fa00000u, 0xb8000000u, 0x7f7fffffu, 0xabcd7bffu, 0xb8000000u, 0x47800000u, 0x477fe000u}, {{{0x7fa00000u, 0x00008000u, 0x477fe000u}, {0x7fa00000u, 0x0000fc00u, 0x477fde00u}, {0x7fa00000u, 0x00008000u, 0x477fe000u}, {0x7fa00000u, 0x0000fc00u, 0x477fde00u}, {0x7fe00000u, 0x00008000u, 0x477fe000u}, {0x7fe00000u, 0x0000fc00u, 0x477fde00u}, {0x7fe00000u, 0x00008000u, 0x477fe000u}, {0x7fe00000u, 0x0000fc00u, 0x477fde00u}}}},
    {{0x33800000u, 0xff800000u, 0xbf800000u, 0x33800000u, 0x3f800000u, 0xabcd7bffu, 0x33800000u, 0xff800000u, 0x477fe000u}, {{{0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00000001u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00000001u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00000001u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00000001u, 0xff800000u}}}},
    {{0xb8000000u, 0x477ff000u, 0x33800000u, 0xb8000000u, 0x3f800000u, 0xabcd8200u, 0xb8000000u, 0x477ff000u, 0xb8000000u}, {{{0x00000000u, 0x00008000u, 0x80000000u}, {0xbffff000u, 0x00008200u, 0xbffff100u}, {0x00000000u, 0x00008000u, 0x80000000u}, {0xbffff000u, 0x00008200u, 0xbffff100u}, {0x00000000u, 0x00008000u, 0x80000000u}, {0xbffff000u, 0x00008200u, 0xbffff100u}, {0x00000000u, 0x00008000u, 0x80000000u}, {0xbffff000u, 0x00008200u, 0xbffff100u}}}},
    {{0x33800000u, 0x7fa00000u, 0xb8000000u, 0x33800000u, 0xff800000u, 0xabcd7bffu, 0x33800000u, 0x7fa00000u, 0x477fe000u}, {{{0x7fa00000u, 0x0000fe00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fe00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fe00000u, 0x0000fe00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fe00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0xff800000u, 0x00000001u, 0x43000000u, 0xff800000u, 0x47800000u, 0xabcd7d00u, 0xff800000u, 0x00000001u, 0x7fa00000u}, {{{0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xff800000u, 0x0000fc00u, 0x7fa00000u}, {0xff800000u, 0x0000fc00u, 0x7fa00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xff800000u, 0x0000fc00u, 0x7fe00000u}, {0xff800000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0x7f800000u, 0x7fa00000u, 0x33800000u, 0x7f800000u, 0x80400000u, 0xabcd3c00u, 0x7f800000u, 0x7fa00000u, 0x3f800000u}, {{{0x7fa00000u, 0x0000fe00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fe00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fe00000u, 0x0000fe00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fe00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0x3f800000u, 0x00000001u, 0x7fa00000u, 0x3f800000u, 0xff800000u, 0xabcd0001u, 0x3f800000u, 0x00000001u, 0x33800000u}, {{{0x7fa00000u, 0x0000fc00u, 0x00000000u}, {0x7fa00000u, 0x0000fc00u, 0x33800000u}, {0x7fa00000u, 0x0000fc00u, 0x00000001u}, {0x7fa00000u, 0x0000fc00u, 0x33800000u}, {0x7fe00000u, 0x0000fc00u, 0x00000000u}, {0x7fe00000u, 0x0000fc00u, 0x33800000u}, {0x7fe00000u, 0x0000fc00u, 0x00000001u}, {0x7fe00000u, 0x0000fc00u, 0x33800000u}}}},
    {{0xb8000000u, 0x7fa00000u, 0x33800000u, 0xb8000000u, 0x4f000000u, 0xabcd7e00u, 0xb8000000u, 0x7fa00000u, 0x7fc00000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x0000fc00u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0xff800000u, 0x80400000u, 0x43000000u, 0xff800000u, 0x477ff000u, 0xabcd7d00u, 0xff800000u, 0x80400000u, 0x7fa00000u}, {{{0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0x7f800000u, 0x0000fc00u, 0x7fa00000u}, {0x7f800000u, 0x0000fc00u, 0x7fa00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0xffc00000u, 0x0000fc00u, 0xffc00000u}, {0x7f800000u, 0x0000fc00u, 0x7fe00000u}, {0x7f800000u, 0x0000fc00u, 0x7fe00000u}}}},
    {{0x33800000u, 0x4f000000u, 0x3f800000u, 0x33800000u, 0x47800000u, 0xabcdbc00u, 0x33800000u, 0x4f000000u, 0xbf800000u}, {{{0x3f800000u, 0x00008000u, 0xbf800000u}, {0x43010000u, 0x00001c00u, 0x42fe0000u}, {0x3f800000u, 0x00008000u, 0xbf800000u}, {0x43010000u, 0x00001c00u, 0x42fe0000u}, {0x3f800000u, 0x00008000u, 0xbf800000u}, {0x43010000u, 0x00001c00u, 0x42fe0000u}, {0x3f800000u, 0x00008000u, 0xbf800000u}, {0x43010000u, 0x00001c00u, 0x42fe0000u}}}},
    {{0x7f800000u, 0x00000001u, 0x3f800000u, 0x7f800000u, 0x80400000u, 0xabcd5800u, 0x7f800000u, 0x00000001u, 0x43000000u}, {{{0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0x7f800000u, 0x0000fc00u, 0x7f800000u}, {0x7f800000u, 0x0000fc00u, 0x7f800000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0xffc00000u, 0x0000fe00u, 0xffc00000u}, {0x7f800000u, 0x0000fc00u, 0x7f800000u}, {0x7f800000u, 0x0000fc00u, 0x7f800000u}}}},
    {{0x33800000u, 0x7fa00000u, 0x7fc00000u, 0x33800000u, 0x3f800000u, 0xabcd7d00u, 0x33800000u, 0x7fa00000u, 0x7fa00000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00000001u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00000001u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00000001u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00000001u, 0x7fe00000u}}}},
    {{0xb8000000u, 0x7fa00000u, 0x477fe000u, 0xb8000000u, 0x3f800000u, 0xabcd0000u, 0xb8000000u, 0x7fa00000u, 0x00000000u}, {{{0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00008200u, 0x7fa00000u}, {0x7fa00000u, 0x00008000u, 0x7fa00000u}, {0x7fa00000u, 0x00008200u, 0x7fa00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00008200u, 0x7fe00000u}, {0x7fe00000u, 0x00008000u, 0x7fe00000u}, {0x7fe00000u, 0x00008200u, 0x7fe00000u}}}},
    {{0x33800000u, 0x4f000000u, 0x7fa00000u, 0x33800000u, 0x477ff000u, 0xabcd8000u, 0x33800000u, 0x4f000000u, 0x80000000u}, {{{0x7fa00000u, 0x00008000u, 0x00000000u}, {0x7fa00000u, 0x00001bffu, 0x43000000u}, {0x7fa00000u, 0x00008000u, 0x00000000u}, {0x7fa00000u, 0x00001bffu, 0x43000000u}, {0x7fe00000u, 0x00008000u, 0x00000000u}, {0x7fe00000u, 0x00001bffu, 0x43000000u}, {0x7fe00000u, 0x00008000u, 0x00000000u}, {0x7fe00000u, 0x00001bffu, 0x43000000u}}}},
    {{0xb8000000u, 0x7f800000u, 0x00000000u, 0xb8000000u, 0x3f800000u, 0xabcd7bffu, 0xb8000000u, 0x7f800000u, 0x477fe000u}, {{{0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00008200u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00008200u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00008200u, 0xff800000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xff800000u, 0x00008200u, 0xff800000u}}}},
    {{0xb8000000u, 0x477ff000u, 0x33800000u, 0xb8000000u, 0xff800000u, 0xabcd0001u, 0xb8000000u, 0x477ff000u, 0x33800000u}, {{{0x00000000u, 0x0000fe00u, 0x00000000u}, {0xbffff000u, 0x00007c00u, 0xbffff000u}, {0x00000000u, 0x0000fe00u, 0x00000000u}, {0xbffff000u, 0x00007c00u, 0xbffff000u}, {0x00000000u, 0x0000fe00u, 0x00000000u}, {0xbffff000u, 0x00007c00u, 0xbffff000u}, {0x00000000u, 0x0000fe00u, 0x00000000u}, {0xbffff000u, 0x00007c00u, 0xbffff000u}}}},
    {{0xb8000000u, 0x00000000u, 0x7fc00000u, 0xb8000000u, 0x7f7fffffu, 0xabcd8200u, 0xb8000000u, 0x00000000u, 0xb8000000u}, {{{0x7fc00000u, 0x00008000u, 0x80000000u}, {0x7fc00000u, 0x0000fc00u, 0xb8000000u}, {0x7fc00000u, 0x00008000u, 0x80000000u}, {0x7fc00000u, 0x0000fc00u, 0xb8000000u}, {0x7fc00000u, 0x00008000u, 0x80000000u}, {0x7fc00000u, 0x0000fc00u, 0xb8000000u}, {0x7fc00000u, 0x00008000u, 0x80000000u}, {0x7fc00000u, 0x0000fc00u, 0xb8000000u}}}},
    {{0xffc02000u, 0x00000001u, 0x7f800000u, 0xffc02000u, 0x7fa00000u, 0xabcd0000u, 0xffc02000u, 0x00000001u, 0x00000000u}, {{{0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}}}},
    {{0xffc02000u, 0x80400000u, 0x3f800000u, 0xffc02000u, 0x47800000u, 0xabcdfe01u, 0xffc02000u, 0x80400000u, 0xffc02000u}, {{{0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}}}},
    {{0x7fa00000u, 0x4f000000u, 0x3f800000u, 0x7fa00000u, 0x7fc00000u, 0xabcdfc00u, 0x7fa00000u, 0x4f000000u, 0xff800000u}, {{{0x7fa00000u, 0x00007f00u, 0x7fa00000u}, {0x7fa00000u, 0x00007f00u, 0x7fa00000u}, {0x7fa00000u, 0x00007f00u, 0x7fa00000u}, {0x7fa00000u, 0x00007f00u, 0x7fa00000u}, {0x7fe00000u, 0x00007f00u, 0x7fe00000u}, {0x7fe00000u, 0x00007f00u, 0x7fe00000u}, {0x7fe00000u, 0x00007f00u, 0x7fe00000u}, {0x7fe00000u, 0x00007f00u, 0x7fe00000u}}}},
    {{0x7f800000u, 0x00000001u, 0xff800000u, 0x7f800000u, 0x477ff000u, 0xabcd7e00u, 0x7f800000u, 0x00000001u, 0x7fc00000u}, {{{0xffc00000u, 0x00007c00u, 0xffc00000u}, {0xffc00000u, 0x00007c00u, 0xffc00000u}, {0xffc00000u, 0x00007c00u, 0x7fc00000u}, {0xffc00000u, 0x00007c00u, 0x7fc00000u}, {0xffc00000u, 0x00007c00u, 0xffc00000u}, {0xffc00000u, 0x00007c00u, 0xffc00000u}, {0xffc00000u, 0x00007c00u, 0x7fc00000u}, {0xffc00000u, 0x00007c00u, 0x7fc00000u}}}},
    {{0xffc02000u, 0x80400000u, 0xb8000000u, 0xffc02000u, 0x00000000u, 0xabcd7c00u, 0xffc02000u, 0x80400000u, 0x7f800000u}, {{{0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}, {0xffc02000u, 0x0000fe01u, 0xffc02000u}}}},
    {{0x7f800000u, 0xffc00001u, 0x7fa00000u, 0x7f800000u, 0xff800000u, 0xabcd0000u, 0x7f800000u, 0xffc00001u, 0x00000000u}, {{{0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}, {0xffc00001u, 0x0000fc00u, 0xffc00001u}}}},
    {{0x80000000u, 0x7f800000u, 0x43000000u, 0x80000000u, 0x3f800000u, 0xabcd0000u, 0x80000000u, 0x7f800000u, 0x00000000u}, {{{0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}, {0xffc00000u, 0x00008000u, 0xffc00000u}}}},
}};

constexpr std::uint32_t Words = 12;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, Rows.size() * Words + 4> Data{};

RecompileResult Compile(std::span<const std::uint32_t> modes, bool integer, bool raw, std::uint32_t words) {
    IrProgram program;
    const ShaderStageInputInfo inputs{};
    SpirvEmitterState state(program, inputs);
    state.integerInterpolation = integer;
    auto& module = state.module;
    module.EmitCapability(spv::CapabilityShader);
    module.EmitCapability(integer ? spv::CapabilityInt64 : spv::CapabilityFloat64);
    module.EmitCapability(spv::CapabilitySignedZeroInfNanPreserve);
    module.EmitExtension("SPV_KHR_float_controls");
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto u32 = TypeU32(state);
    const auto array = module.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, u32);
    const auto structure = module.Type(spv::OpTypeStruct, array);
    module.AddAnnotation(spv::OpDecorate, structure, spv::DecorationBlock);
    module.AddAnnotation(spv::OpMemberDecorate, structure, 0u, spv::DecorationOffset, 0u);
    const auto buffer = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, structure), spv::StorageClassStorageBuffer);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationDescriptorSet, 0u);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationBinding, 0u);
    const auto invocation = module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, TypeU32Vector(state, 3u)), spv::StorageClassInput);
    module.AddAnnotation(spv::OpDecorate, invocation, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
    const auto element = module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, u32);
    const auto main = module.AllocateId();
    const auto voidType = module.Type(spv::OpTypeVoid);
    module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {invocation});
    module.AddExecutionMode(main, spv::ExecutionModeLocalSize, static_cast<std::uint32_t>(Rows.size()), 1u, 1u);
    module.AddExecutionMode(main, spv::ExecutionModeSignedZeroInfNanPreserve, 32u);
    if (!integer) module.AddExecutionMode(main, spv::ExecutionModeSignedZeroInfNanPreserve, 64u);
    module.AddFunction(spv::OpFunction, voidType, main, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto id = module.AllocateId();
    module.AddFunction(spv::OpLoad, TypeU32Vector(state, 3u), id, invocation);
    const auto lane = module.AllocateId();
    module.AddFunction(spv::OpCompositeExtract, u32, lane, id, 0u);
    const auto rowBase = Binary(state, spv::OpIMul, u32, lane, ConstantU32(state, words));
    const auto address = [&](std::uint32_t offset) {
        const auto pointer = module.AllocateId();
        module.AddFunction(spv::OpAccessChain, element, pointer, buffer, ConstantU32(state, 0u), Binary(state, spv::OpIAdd, u32, rowBase, ConstantU32(state, offset)));
        return pointer;
    };
    const auto load = [&](std::uint32_t offset) {
        const auto word = module.AllocateId();
        module.AddFunction(spv::OpLoad, u32, word, address(offset));
        return Unary(state, spv::OpBitcast, TypeF32(state), word);
    };
    IrProgram constants;
    IrBuilder builder(constants);
    builder.SetInsertionPoint(constants.CreateBlock());
    std::array<std::uint32_t, 9> operands{};
    for (std::uint32_t index = 0; index < operands.size(); ++index) operands[index] = load(index);
    for (std::uint32_t slot = 0; slot < modes.size(); ++slot) {
        const auto* flags = &builder.Constant(modes[slot]);
        for (std::uint32_t op = 0; op < 3u; ++op) {
            const auto delta = operands[op * 3u];
            const auto coordinate = operands[op * 3u + 1u];
            const auto base = operands[op * 3u + 2u];
            const auto value = op == 1u ? EmitFPInterpolateF16(state, delta, coordinate, base, flags) : EmitFPInterpolateF32(state, delta, coordinate, base, flags);
            const auto result = op == 1u && !raw ? EmitConvertF16F32(state, value) : Unary(state, spv::OpBitcast, u32, value);
            module.AddFunction(spv::OpStore, address(9u + slot * 3u + op), result);
        }
    }
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    RecompileResult result{};
    result.spirv = module.Finalize();
    const auto base = reinterpret_cast<std::uintptr_t>(Data.data());
    result.bindings.push_back({DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0u, 0u, 1u,
        {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>((base >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Data)), 0x31016facu}, false});
    return result;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t mode, bool integer) {
    Data.fill(Sentinel);
    for (std::uint32_t row = 0; row < Rows.size(); ++row) std::copy(Rows[row].input.begin(), Rows[row].input.end(), Data.begin() + row * Words);
    const std::array<std::uint32_t, 1> modes{ModeFlags[mode]};
    device.Dispatch(Compile(modes, integer, false, Words), 1u, 1u, 1u);
    device.WaitIdle();
    for (std::uint32_t row = 0; row < Rows.size(); ++row) {
        for (std::uint32_t input = 0; input < 9u; ++input) Require(Data[row * Words + input] == Rows[row].input[input], "interpolation modes changed an input");
        for (std::uint32_t op = 0; op < 3u; ++op) {
            const auto actual = Data[row * Words + 9u + op];
            const auto expected = Rows[row].expected[mode][op];
            char message[200];
            std::snprintf(message, sizeof(message), "%s (%s), %s, row %u: got 0x%08x, expected 0x%08x", OpNames[op], integer ? "integer" : "Float64", ModeNames[mode], row, actual, expected);
            Require(actual == expected, message);
        }
    }
    for (std::size_t index = Rows.size() * Words; index < Data.size(); ++index) Require(Data[index] == Sentinel, "interpolation modes wrote beyond the rows");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto target = device->Target();
        const bool float64 = std::find(target.supportedCapabilities.begin(), target.supportedCapabilities.end(), spv::CapabilityFloat64) != target.supportedCapabilities.end();
        for (std::uint32_t mode = 0; mode < ModeFlags.size(); ++mode) {
            Run(*device, mode, true);
            if (float64) Run(*device, mode, false);
        }
        std::puts(float64 ? "interpolation modes passed: 32 hardware rows, 3 ops, 8 float modes, integer and Float64 paths"
                          : "interpolation modes passed: 32 hardware rows, 3 ops, 8 float modes, integer path (the device has no Float64)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
