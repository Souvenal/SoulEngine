/// @file   Vulkan/VKDebug.cppm
/// @brief  Vulkan debug-utils helpers.

module;
#include <vulkan/vulkan.h>

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

    /// Instance-create-info chain extension for debug messages when enabled.
    [[nodiscard]] static auto PrepareInstanceChain() -> std::optional<vk::DebugUtilsMessengerCreateInfoEXT> {
        if (!IsConfigEnabled())
            return std::nullopt;
        return CreateDebugMessengerCI();
    }

    /// Create the standalone debug messenger after the instance exists.
    [[nodiscard]] auto InitializeMessenger(vk::raii::Instance& Instance) -> std::expected<void, ErrorMessage> {
        if (!IsConfigEnabled())
            return {};
        auto DebugMessenger = Instance.createDebugUtilsMessengerEXT(CreateDebugMessengerCI(), nullptr);
        if (DebugMessenger.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("Failed to create Vulkan debug messenger: {}", vk::to_string(DebugMessenger.result))));
        m_Messenger = std::move(DebugMessenger.value);
        return {};
    }

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

    [[nodiscard]] auto IsEnabled() const noexcept -> bool { return m_Enabled; }

    [[nodiscard]] static auto IsConfigEnabled() -> bool {
        return ConfigManager::Get().GetConfig().RhiVulkan.DebugUtils.value_or(true);
    }

  private:
    [[nodiscard]] static auto CreateDebugMessengerCI() -> vk::DebugUtilsMessengerCreateInfoEXT {
        return vk::DebugUtilsMessengerCreateInfoEXT{
            .messageSeverity =
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = &VulkanDebugCallback,
        };
    }

    static VKAPI_ATTR auto VKAPI_CALL VulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT      MessageSeverity,
                                                           vk::DebugUtilsMessageTypeFlagsEXT             MessageTypes,
                                                           const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
                                                           void*) -> vk::Bool32 {
        if (!CallbackData) {
            LogError("[Vulkan] Debug callback data is empty");
            return vk::True;
        }
        const StringView MessageId = CallbackData->pMessageIdName ? CallbackData->pMessageIdName : "UnknownMessage";
        const StringView Message   = CallbackData->pMessage ? CallbackData->pMessage : "No Vulkan debug message";
        const auto Types = vk::to_string(MessageTypes);
        String DetailedMessage{Message};
        if (CallbackData->objectCount > 0) {
            DetailedMessage += Format("\nObjects: {}", CallbackData->objectCount);
            for (Uint32 Index = 0; Index < CallbackData->objectCount; ++Index) {
                const auto& Object = CallbackData->pObjects[Index];
                DetailedMessage +=
                    Format("\n    [{}] Vk{} 0x{:x}", Index, vk::to_string(Object.objectType), Object.objectHandle);
                if (Object.pObjectName)
                    DetailedMessage += Format("[{}]", Object.pObjectName);
            }
        }
        switch (MessageSeverity) {
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
            LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
            // There are too much `eInfo` messages,
            // so we use `LogDebug`.
            LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
            LogWarning("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
            LogError("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        }
        return vk::False;
    }

    vk::raii::Device*                m_Device    = nullptr;
    bool                             m_Enabled   = false;
    vk::raii::DebugUtilsMessengerEXT m_Messenger = nullptr;
};

} // namespace SoulEngine
