/// @file   Vulkan/VKDebug.cppm
/// @brief  Vulkan debug-utils helpers.

export module Vulkan:Debug;

import Core;
import vulkan;
import std;

import :Capability;

export namespace SoulEngine {

/// Device-scoped VK_EXT_debug_utils service.
class VulkanDebugUtils final {
  public:
    VulkanDebugUtils() = default;

    auto Initialize(vk::raii::Device& Device) -> void {
        m_Device  = &Device;
        m_Enabled = VulkanCapability::Get().IsInstanceExtensionEnabled(vk::EXTDebugUtilsExtensionName);
    }

    template <typename T>
        requires vk::isVulkanHandleType<std::remove_cvref_t<T>>::value
    auto SetObjectName(T ObjectHandle, StringView Name) const -> void {
        using HandleType = std::remove_cvref_t<T>;

        constexpr vk::ObjectType ObjectType = HandleType::objectType;
        const Uint64             Handle     = std::bit_cast<Uint64>(ObjectHandle);
        if (!m_Device || !m_Enabled || Name.empty() || Handle == 0)
            return;

        const String ObjectName(Name);
        const auto   Result = m_Device->setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT{
            .objectType   = ObjectType,
            .objectHandle = Handle,
            .pObjectName  = ObjectName.c_str(),
        });
        if (Result != vk::Result::eSuccess)
            LogWarning("Failed to name Vulkan object '{}': {}", ObjectName, vk::to_string(Result));
    }

  private:
    vk::raii::Device* m_Device  = nullptr;
    bool              m_Enabled = false;
};

} // namespace SoulEngine
