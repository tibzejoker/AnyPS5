#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEVICEDIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEVICEDIAGNOSTICS_HPP

#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver::Graphics {

// APS5_GPU_CHECKPOINTS=1: the device is created with VK_EXT_device_fault and
// VK_NV_device_diagnostic_checkpoints when the driver has them; every recorded draw and dispatch
// sets a checkpoint naming its program and serial, and a lost device prints the fault the driver
// reports and the last checkpoints the queue reached ([device-lost]).
bool GpuCheckpointsRequested();
void InitDeviceDiagnostics(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr deviceProc, bool deviceFault, bool checkpoints);
enum class CheckpointKind : std::uint8_t { Draw, Dispatch };
void NoteCheckpoint(VkCommandBuffer commands, CheckpointKind kind, std::uint64_t programAddress);
// Prints the [device-lost] report once per device (from Check on VK_ERROR_DEVICE_LOST).
void ReportDeviceLost();
// VK_EXT_device_address_binding_report (APS5_GPU_CHECKPOINTS): the debug messenger callback that keeps
// every GPU virtual address bind and unbind, searched for the fault addresses when the device is lost.
VKAPI_ATTR VkBool32 VKAPI_CALL AddressBindingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

}

#endif
