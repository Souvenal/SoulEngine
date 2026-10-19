/// @file   Vulkan/VKDebug.cppm
/// @brief  Vulkan debug-utils helpers.

module;
#include <vulkan/vulkan.h>

export module Vulkan:Debug;

import Core;
import vulkan;
import std;

import :Capability;

namespace SoulEngine {

/// Device-scoped VK_EXT_debug_utils service.
export class VulkanDebugUtils final {
  public:
    VulkanDebugUtils() = default;

    /// Instance-create-info chain extension for debug messages when enabled.
    [[nodiscard]] static auto PrepareInstanceChain() -> std::optional<vk::DebugUtilsMessengerCreateInfoEXT>;

    /// Create the standalone debug messenger after the instance exists.
    [[nodiscard]] auto InitializeMessenger(vk::raii::Instance& Instance) -> std::expected<void, ErrorMessage>;

    auto Initialize(vk::raii::Device& Device) -> void;

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

    [[nodiscard]] auto IsEnabled() const noexcept -> bool;

    [[nodiscard]] static auto IsConfigEnabled() -> bool;

  private:
    [[nodiscard]] static auto CreateDebugMessengerCI() -> vk::DebugUtilsMessengerCreateInfoEXT;

    static VKAPI_ATTR auto VKAPI_CALL VulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT      MessageSeverity,
                                                          vk::DebugUtilsMessageTypeFlagsEXT             MessageTypes,
                                                          const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
                                                          void*) -> vk::Bool32;

    vk::raii::Device*                m_Device    = nullptr;
    bool                             m_Enabled   = false;
    vk::raii::DebugUtilsMessengerEXT m_Messenger = nullptr;
};

} // namespace SoulEngine
