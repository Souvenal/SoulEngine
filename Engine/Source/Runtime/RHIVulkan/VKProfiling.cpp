module;
#include <tracy/Tracy.hpp>
#include <vulkan/vulkan.h>
#include <tracy/TracyVulkan.hpp>

module RHIVulkan.Profiling;

import Core;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Context;
import RHIVulkan.Debug;

namespace SoulEngine {

auto CollectGpu(VulkanResourceContext& ResourceContext, vk::raii::CommandBuffer& CommandBuffer) -> void {
    auto* Ctx = ResourceContext.GetTracyCtx();
    if (!Ctx)
        return;
    Ctx->Collect(static_cast<VkCommandBuffer>(*CommandBuffer));
}

LabelScope::LabelScope(const VulkanResourceContext& ResourceContext,
                       vk::raii::CommandBuffer&     CommandBuffer,
                       StringView                   Name,
                       std::source_location         SourceLocation)
    : m_ResourceContext(ResourceContext),
      m_CommandBuffer(CommandBuffer),
      m_Zone(TracyCtx(ResourceContext),
             &GetZoneSourceLocation(Name, SourceLocation),
             static_cast<VkCommandBuffer>(*CommandBuffer),
             TracyCtx(ResourceContext) != nullptr && !Name.empty()) {
    if (m_ResourceContext.GetDebugUtils().IsEnabled())
        BeginLabel(CommandBuffer, Name);
}

LabelScope::~LabelScope() {
    if (m_ResourceContext.GetDebugUtils().IsEnabled())
        m_CommandBuffer.endDebugUtilsLabelEXT();
}

auto LabelScope::TracyCtx(const VulkanResourceContext& ResourceContext) -> tracy::VkCtx* {
    return ResourceContext.GetTracyCtx();
}

void LabelScope::BeginLabel(vk::raii::CommandBuffer& CommandBuffer, StringView Name) {
    const String LabelName(Name);
    CommandBuffer.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = LabelName.c_str(),
    });
}

auto LabelScope::GetZoneSourceLocation(StringView Name, const std::source_location& SourceLocation)
    -> const tracy::SourceLocationData& {
    static const tracy::SourceLocationData Empty{"", "", "", 0, 0};
    struct StringHash {
        using is_transparent = void;
        auto operator()(StringView Value) const -> Uint64 {
            return std::hash<StringView>{}(Value);
        }
    };
    struct StringEqual {
        using is_transparent = void;
        auto operator()(StringView Lhs, StringView Rhs) const -> bool {
            return Lhs == Rhs;
        }
    };
    static std::unordered_map<String, tracy::SourceLocationData, StringHash, StringEqual> ZoneSourceLocations;
    if (Name.empty())
        return Empty;
    if (const auto It = ZoneSourceLocations.find(Name); It != ZoneSourceLocations.end())
        return It->second;
    auto [Iterator, Inserted] = ZoneSourceLocations.try_emplace(String{Name});
    if (Inserted)
        Iterator->second = tracy::SourceLocationData{Iterator->first.c_str(),
                                                     SourceLocation.function_name(),
                                                     SourceLocation.file_name(),
                                                     SourceLocation.line(),
                                                     0};
    return Iterator->second;
}

} // namespace SoulEngine
