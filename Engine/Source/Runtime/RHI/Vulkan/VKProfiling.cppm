/// @file   Vulkan/VKProfiling.cppm
/// @brief  RAII command-buffer annotation scope: debug label + Tracy GPU zone.
///
///         This partition is the ONLY place in the Vulkan module that may
///         include Tracy headers. Tracy's headers transitively pull
///         <thread>/<stop_token>; keeping them out of Vulkan:Context's global
///         module fragment keeps Context.ifc free of the std-internal
///         reference records that trip MSVC's IFC merge (C1116, VS 2026 14.51).
///
///         Runtime note: the Tracy VkCtx GPU timestamps cycle through a
///         64K-slot query ring; Collect() recycles consumed ranges with a
///         vkCmdResetQueryPool recorded into the frame's primary command
///         buffer, while the vkCmdWriteTimestamp calls land in per-pass
///         secondary command buffers. Validation layers without the VVL
///         cross-command-buffer query-state fix (KhronosGroup/
///         Vulkan-ValidationLayers #8233, fixed by PR #13116) report
///         false-positive VUID-vkCmdWriteTimestamp-None-00830 "query not
///         reset" errors once the ring first wraps (~64K timestamps,
///         roughly half a minute at 60 fps). These are benign and can be
///         filtered by message ID 0xeb0b9b05; see also wolfpld/tracy#663.

module;
#include <tracy/Tracy.hpp>
#include <vulkan/vulkan.h>
#include <tracy/TracyVulkan.hpp>

export module Vulkan:Profiling;

import Core;
import vulkan;
import std;

import :Capability;
import :Context;
import :Debug;

export namespace SoulEngine {

/// Initialize the Tracy GPU profiling context with calibrated timestamps.
/// No-op when VK_EXT_calibrated_timestamps is unavailable.
auto InitializeGpuProfiling(VulkanResourceContext& ResourceContext) -> void {
    if (!VulkanCapability::Get().IsDeviceExtensionEnabled(vk::EXTCalibratedTimestampsExtensionName))
        return;
    // TRACY_VK_USE_SYMBOL_TABLE (target define) routes every Tracy Vulkan call
    // through a symbol table populated from the instance dispatcher below, so
    // this TU never emits extern loader symbols and vulkan-1.lib stays unlinked.
    const auto& Dispatcher = *ResourceContext.GetInstance().getDispatcher();
    const auto  InstanceProc = Dispatcher.vkGetInstanceProcAddr;
    const auto  DeviceProc   = Dispatcher.vkGetDeviceProcAddr;
    const auto PDCTD = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
        InstanceProc(static_cast<VkInstance>(*ResourceContext.GetInstance()),
            "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    const auto GCT = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
        DeviceProc(static_cast<VkDevice>(*ResourceContext.GetDevice()),
            "vkGetCalibratedTimestampsEXT"));
    if (!PDCTD || !GCT)
        return;
    const vk::CommandPoolCreateInfo PoolCI{
        .flags            = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = ResourceContext.GetGraphicsFamily(),
    };
    auto PoolRes = ResourceContext.GetDevice().createCommandPool(PoolCI);
    if (PoolRes.result != vk::Result::eSuccess)
        return;
    const vk::CommandBufferAllocateInfo AllocCI{
        .commandPool        = *PoolRes.value,
        .level              = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    auto InitCmdRes = ResourceContext.GetDevice().allocateCommandBuffers(AllocCI);
    if (InitCmdRes.result != vk::Result::eSuccess)
        return;
    ResourceContext.SetTracyCtx(tracy::CreateVkContext(
        static_cast<VkInstance>(*ResourceContext.GetInstance()),
        static_cast<VkPhysicalDevice>(*ResourceContext.GetPhysicalDevice()),
        static_cast<VkDevice>(*ResourceContext.GetDevice()),
        static_cast<VkQueue>(*ResourceContext.GetGraphicsQueue()),
        static_cast<VkCommandBuffer>(*InitCmdRes.value[0]), InstanceProc, DeviceProc, true));
}

/// Collect pending Tracy GPU zone data into the given command buffer.
auto CollectGpu(VulkanResourceContext& ResourceContext, vk::raii::CommandBuffer& CommandBuffer) -> void {
    auto* Ctx = static_cast<tracy::VkCtx*>(ResourceContext.GetTracyCtx());
    if (!Ctx)
        return;
    Ctx->Collect(static_cast<VkCommandBuffer>(*CommandBuffer));
}

/// Destroy the Tracy GPU profiling context. Must run while the device is alive.
auto ShutdownGpuProfiling(VulkanResourceContext& ResourceContext) -> void {
    auto* Ctx = static_cast<tracy::VkCtx*>(ResourceContext.GetTracyCtx());
    if (!Ctx)
        return;
    tracy::DestroyVkContext(Ctx);
    ResourceContext.SetTracyCtx(nullptr);
}

/// RAII command-buffer annotation: emits a VK_EXT_debug_utils label and a
/// Tracy GPU zone for the same region. The label is emitted when debug utils
/// are enabled (checked at construction and destruction); the zone no-ops
/// when no Tracy GPU context is available or the name is empty.
class LabelScope final {
  public:
    LabelScope(VulkanResourceContext& ResourceContext, vk::raii::CommandBuffer& CommandBuffer,
               StringView Name, std::source_location SourceLocation = std::source_location::current())
        : m_ResourceContext(ResourceContext)
        , m_CommandBuffer(CommandBuffer)
        , m_Zone(TracyCtx(ResourceContext), &GetZoneSourceLocation(Name, SourceLocation),
            static_cast<VkCommandBuffer>(*CommandBuffer), TracyCtx(ResourceContext) != nullptr && !Name.empty()) {
        if (m_ResourceContext.GetDebugUtils().IsEnabled())
            BeginLabel(CommandBuffer, Name);
    }

    ~LabelScope() {
        if (m_ResourceContext.GetDebugUtils().IsEnabled())
            m_CommandBuffer.endDebugUtilsLabelEXT();
    }

    LabelScope(const LabelScope&)            = delete;
    LabelScope& operator=(const LabelScope&) = delete;
    LabelScope(LabelScope&&)                 = delete;
    LabelScope& operator=(LabelScope&&)      = delete;

  private:
    [[nodiscard]] static auto TracyCtx(VulkanResourceContext& ResourceContext) -> tracy::VkCtx* {
        // Type-erased on VulkanResourceContext so its TU never sees Tracy headers.
        return static_cast<tracy::VkCtx*>(ResourceContext.GetTracyCtx());
    }

    static void BeginLabel(vk::raii::CommandBuffer& CommandBuffer, StringView Name) {
        const String LabelName(Name);
        CommandBuffer.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = LabelName.c_str(),
        });
    }

    static const tracy::SourceLocationData& GetZoneSourceLocation(StringView Name, const std::source_location& SourceLocation) {
        static const tracy::SourceLocationData Empty{"", "", "", 0, 0};
        struct StringHash {
            using is_transparent = void;
            auto operator()(StringView Value) const -> Uint64 { return std::hash<StringView>{}(Value); }
        };
        struct StringEqual {
            using is_transparent = void;
            auto operator()(StringView Lhs, StringView Rhs) const -> bool { return Lhs == Rhs; }
        };
        static std::unordered_map<String, tracy::SourceLocationData, StringHash, StringEqual> ZoneSourceLocations;
        if (Name.empty())
            return Empty;
        if (const auto It = ZoneSourceLocations.find(Name); It != ZoneSourceLocations.end())
            return It->second;
        auto [Iterator, Inserted] = ZoneSourceLocations.try_emplace(String{Name});
        if (Inserted)
            Iterator->second = tracy::SourceLocationData{ Iterator->first.c_str(), SourceLocation.function_name(),
                SourceLocation.file_name(), SourceLocation.line(), 0 };
        return Iterator->second;
    }

    VulkanResourceContext&   m_ResourceContext;
    vk::raii::CommandBuffer& m_CommandBuffer;
    tracy::VkCtxScope        m_Zone;
};

} // namespace SoulEngine
