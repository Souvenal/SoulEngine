module RHIVulkan.Debug;

import Core;
import vulkan;
import std;

import RHIVulkan.Capability;

namespace SoulEngine {

CVar<bool> CVarRhiVulkanDebugUtils{
    "RHI.Vulkan.DebugUtils", true, "Enable VK_EXT_debug_utils and the debug messenger", CVarTiming::Startup};

auto VulkanDebugUtils::PrepareInstanceChain() -> std::optional<vk::DebugUtilsMessengerCreateInfoEXT> {
    if (!IsConfigEnabled())
        return std::nullopt;
    return CreateDebugMessengerCI();
}

auto VulkanDebugUtils::InitializeMessenger(vk::raii::Instance& Instance) -> std::expected<void, ErrorMessage> {
    if (!IsConfigEnabled())
        return {};
    auto DebugMessenger = Instance.createDebugUtilsMessengerEXT(CreateDebugMessengerCI(), nullptr);
    if (DebugMessenger.result != vk::Result::eSuccess)
        return std::unexpected(
            ErrorMessage(Format("Failed to create Vulkan debug messenger: {}", vk::to_string(DebugMessenger.result))));
    m_Messenger = std::move(DebugMessenger.value);
    return {};
}

auto VulkanDebugUtils::Initialize(vk::raii::Device& Device) -> void {
    m_Device  = &Device;
    m_Enabled = VulkanCapability::Get().IsInstanceExtensionEnabled(vk::EXTDebugUtilsExtensionName);
}

auto VulkanDebugUtils::IsEnabled() const noexcept -> bool {
    return m_Enabled;
}

auto VulkanDebugUtils::IsConfigEnabled() -> bool {
    return CVarRhiVulkanDebugUtils.Get();
}

auto VulkanDebugUtils::CreateDebugMessengerCI() -> vk::DebugUtilsMessengerCreateInfoEXT {
    return vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity =
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = &VulkanDebugCallback,
    };
}

auto VulkanDebugUtils::VulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT      MessageSeverity,
                                           vk::DebugUtilsMessageTypeFlagsEXT             MessageTypes,
                                           const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
                                           void*) -> vk::Bool32 {
    if (!CallbackData) {
        LogError("[Vulkan] Debug callback data is empty");
        return vk::True;
    }
    const StringView MessageId = CallbackData->pMessageIdName ? CallbackData->pMessageIdName : "UnknownMessage";
    const StringView Message   = CallbackData->pMessage ? CallbackData->pMessage : "No Vulkan debug message";
    const auto       Types     = vk::to_string(MessageTypes);
    String           DetailedMessage{Message};
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
        // There are too much `eInfo` messages, so we use `LogDebug`.
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

} // namespace SoulEngine
