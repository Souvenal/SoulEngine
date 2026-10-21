/// @file   RHIVulkan/VKProfiling.cppm
/// @brief  RAII command-buffer annotation scope: debug label + Tracy GPU zone.
///
///         Tracy's Vulkan context is owned by Vulkan:Context; this partition
///         records GPU zones and debug labels against that typed context.
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

export module RHIVulkan.Profiling;

import Core;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Context;
import RHIVulkan.Debug;

namespace SoulEngine {

/// Collect pending Tracy GPU zone data into the given command buffer.
export auto CollectGpu(VulkanResourceContext& ResourceContext, vk::raii::CommandBuffer& CommandBuffer) -> void;

/// @brief RAII command-buffer annotation: debug label and Tracy GPU zone for the same region.
///
/// Emits a VK_EXT_debug_utils label and a Tracy GPU zone. The label is emitted when debug utils are enabled
/// (checked at construction and destruction); the zone no-ops when no Tracy GPU context is available or the
/// name is empty.
///
/// @note Validation-layer false positive: every pass zone recorded through
///       this scope writes its timestamps into a *secondary* command buffer,
///       while CollectGpu() records the ring-wraparound resets into the
///       frame's *primary* command buffer. VVL up to Vulkan SDK 1.4.357 (tag
///       dated 2026-07-27) does not propagate query state across command
///       buffers within one submission and misreports this as
///       VUID-vkCmdWriteTimestamp-None-00830 (MessageID 0xeb0b9b05). Fixed
///       upstream by KhronosGroup/Vulkan-ValidationLayers PR #13116 (merged
///       2026-09-16, closes #8233); no released SDK ships it yet. Re-test
///       after an SDK update and drop this note once clean. A zone recorded
///       on the primary CB (same CB as the resets) was experimentally
///       confirmed to never trigger the report (2026-09-21).
export class LabelScope final {
  public:
    LabelScope(const VulkanResourceContext& ResourceContext,
               vk::raii::CommandBuffer&     CommandBuffer,
               StringView                   Name,
               std::source_location         SourceLocation = std::source_location::current());

    ~LabelScope();

    LabelScope(const LabelScope&)            = delete;
    LabelScope& operator=(const LabelScope&) = delete;
    LabelScope(LabelScope&&)                 = delete;
    LabelScope& operator=(LabelScope&&)      = delete;

  private:
    [[nodiscard]] static auto TracyCtx(const VulkanResourceContext& ResourceContext) -> tracy::VkCtx*;

    static void BeginLabel(vk::raii::CommandBuffer& CommandBuffer, StringView Name);

    static const tracy::SourceLocationData& GetZoneSourceLocation(StringView                  Name,
                                                                  const std::source_location& SourceLocation);

    const VulkanResourceContext& m_ResourceContext;
    vk::raii::CommandBuffer&     m_CommandBuffer;
    tracy::VkCtxScope            m_Zone;
};

} // namespace SoulEngine
