module RHIVulkan.Capability;

import Core;
import vulkan;
import std;

namespace SoulEngine {

CVar<bool> CVarRhiVulkanValidation{"RHI.Vulkan.Validation", true, "Enable VK_LAYER_KHRONOS_validation"};

auto VulkanCapability::AddRequiredRequests(std::vector<VulkanCapabilityRequest>& Requests,
                                           std::span<const char*>                RequiredNames) -> void {
    for (const auto* RequiredName : RequiredNames) {
        auto It = std::ranges::find_if(Requests, [&](const auto& Request) {
            return Request.Name && std::strcmp(Request.Name, RequiredName) == 0;
        });
        if (It != Requests.end()) {
            It->Required = true;
            It->Enabled  = false;
        } else {
            Requests.emplace_back(RequiredName, true, false);
        }
    }
}

auto VulkanCapability::ResolveInstanceExtensions(vk::raii::Context& Ctx, std::span<const char*> RequiredExtensions)
    -> std::expected<std::vector<const char*>, ErrorMessage> {
    auto InstanceExtensions = m_InstanceExts;
    AddRequiredRequests(InstanceExtensions, RequiredExtensions);

    // Enumerate available instance extensions
    auto ExtPropsRes = Ctx.enumerateInstanceExtensionProperties();
    if (ExtPropsRes.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("Failed to enumerate Vulkan instance extension properties"));
    auto& ExtProps = ExtPropsRes.value;

    auto EnabledExtensions = MatchRequests(
        InstanceExtensions,
        ExtProps,
        [](const vk::ExtensionProperties& Prop) { return static_cast<const char*>(Prop.extensionName); },
        "extensions");
    if (!EnabledExtensions)
        return std::unexpected(EnabledExtensions.error());
    m_EnabledInstanceExtensionNames = *EnabledExtensions;
    return m_EnabledInstanceExtensionNames;
}

auto VulkanCapability::ResolveInstanceLayers(vk::raii::Context& Ctx)
    -> std::expected<std::vector<const char*>, ErrorMessage> {
    std::vector<VulkanCapabilityRequest> InstanceLayers;
    const bool                           Validation = CVarRhiVulkanValidation.Get();
    if (!Validation)
        return {};
    const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";
    AddRequiredRequests(InstanceLayers, {&ValidationLayer, 1});

    auto LayerPropsRes = Ctx.enumerateInstanceLayerProperties();
    if (LayerPropsRes.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("Failed to enumerate Vulkan instance layer properties"));
    auto& LayerProps = LayerPropsRes.value;

    return MatchRequests(
        InstanceLayers,
        LayerProps,
        [](const vk::LayerProperties& Prop) { return static_cast<const char*>(Prop.layerName); },
        "layers");
}

auto VulkanCapability::ResolveDeviceExtensionsAndFeatures(vk::raii::PhysicalDevice& PD)
    -> std::expected<std::tuple<std::vector<const char*>, const VulkanFeaturesChain&>, ErrorMessage> {
    // Enumerate available device extensions
    auto ExtPropsRes = PD.enumerateDeviceExtensionProperties();
    if (ExtPropsRes.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("Failed to enumerate device extension properties"));
    auto& ExtProps = ExtPropsRes.value;

    // Match extensions
    auto Result = MatchRequests(
        m_DeviceExts,
        ExtProps,
        [](const vk::ExtensionProperties& Prop) { return static_cast<const char*>(Prop.extensionName); },
        "extensions");
    if (!Result)
        return std::unexpected(Result.error());
    m_EnabledDeviceNames = std::move(*Result);

    // Query supported features from physical device.
    m_SupportedFeatures = PD.getFeatures2<vk::PhysicalDeviceFeatures2,
                                          vk::PhysicalDeviceVulkan11Features,
                                          vk::PhysicalDeviceVulkan12Features,
                                          vk::PhysicalDeviceVulkan13Features,
                                          vk::PhysicalDeviceVulkan14Features,
                                          vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                          vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();

    // Hardware ray tracing is an optional all-or-nothing bundle: match the RT extensions against
    // availability, then keep them only when every extension AND the required features are present. The
    // supported features chain is handed to device creation wholesale, so supported RT features are
    // enabled automatically once the extensions are on.
    if (auto RtMatch = MatchRequests(
            m_RayTracingExts,
            ExtProps,
            [](const vk::ExtensionProperties& Prop) { return static_cast<const char*>(Prop.extensionName); },
            "ray-tracing extensions");
        !RtMatch)
        return std::unexpected(RtMatch.error());
    const auto& V12             = m_SupportedFeatures.get<vk::PhysicalDeviceVulkan12Features>();
    const auto& AsFeatures      = m_SupportedFeatures.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>();
    const auto& RtFeatures      = m_SupportedFeatures.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
    const bool  AllRtExtensions = std::ranges::all_of(m_RayTracingExts, [](const auto& Ext) { return Ext.Enabled; });
    const bool  RtSupported = AllRtExtensions && AsFeatures.accelerationStructure && RtFeatures.rayTracingPipeline &&
                              V12.bufferDeviceAddress && V12.descriptorIndexing;
    if (!RtSupported) {
        for (auto& Ext : m_RayTracingExts)
            Ext.Enabled = false;
        LogWarning("Hardware ray tracing unavailable (extensions: {}, accelerationStructure: {}, "
                   "rayTracingPipeline: {}, bufferDeviceAddress: {}, descriptorIndexing: {})",
                   AllRtExtensions,
                   static_cast<bool>(AsFeatures.accelerationStructure),
                   static_cast<bool>(RtFeatures.rayTracingPipeline),
                   static_cast<bool>(V12.bufferDeviceAddress),
                   static_cast<bool>(V12.descriptorIndexing));
    } else {
        LogInfo("Hardware ray tracing enabled");
    }

    // Add ray tracing extensions if they are available
    for (const auto& Ext : m_RayTracingExts) {
        if (Ext.Enabled)
            m_EnabledDeviceNames.emplace_back(Ext.Name);
    }

    return std::tuple<std::vector<const char*>, const VulkanFeaturesChain&>{m_EnabledDeviceNames, m_SupportedFeatures};
}

auto VulkanCapability::ResolveDeviceProperties(vk::raii::PhysicalDevice& PD) -> void {
    m_Properties = PD.getProperties2<vk::PhysicalDeviceProperties2,
                                     vk::PhysicalDeviceVulkan11Properties,
                                     vk::PhysicalDeviceVulkan12Properties,
                                     vk::PhysicalDeviceVulkan13Properties,
                                     vk::PhysicalDeviceVulkan14Properties,
                                     vk::PhysicalDeviceAccelerationStructurePropertiesKHR,
                                     vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
}

auto VulkanCapability::GetFeatures() const -> const vk::PhysicalDeviceFeatures& {
    return m_SupportedFeatures.get<vk::PhysicalDeviceFeatures2>().features;
}

auto VulkanCapability::GetProperties() const -> const vk::PhysicalDeviceProperties& {
    return m_Properties.get<vk::PhysicalDeviceProperties2>().properties;
}

auto VulkanCapability::IsInstanceExtensionEnabled(const char* Name) -> bool {
    return std::ranges::any_of(m_EnabledInstanceExtensionNames,
                               [Name](const auto* EnabledName) { return std::strcmp(EnabledName, Name) == 0; });
}

auto VulkanCapability::IsDeviceExtensionEnabled(const char* Name) -> bool {
    const auto IsEnabled = [Name](const std::vector<VulkanCapabilityRequest>& Extensions) -> bool {
        auto It = std::ranges::find_if(Extensions,
                                       [Name](const auto& E) { return E.Name && std::strcmp(E.Name, Name) == 0; });
        return It != Extensions.end() && It->Enabled;
    };
    return IsEnabled(m_DeviceExts) || IsEnabled(m_RayTracingExts);
}

auto VulkanCapability::IsRayTracingAvailable() -> bool {
    return IsDeviceExtensionEnabled(vk::KHRAccelerationStructureExtensionName);
}

VulkanCapability::VulkanCapability() {
    RegisterInstanceExtensions();
    RegisterDeviceExtensions();
}

auto VulkanCapability::RegisterInstanceExtensions() -> void {
    m_InstanceExts.emplace_back(vk::KHRPortabilityEnumerationExtensionName, false);
}

auto VulkanCapability::RegisterDeviceExtensions() -> void {
    // VK_KHR_swapchain — required for presenting
    m_DeviceExts.push_back({vk::KHRSwapchainExtensionName, true});
    // VK_KHR_portability_subset — needed for MoltenVK
    m_DeviceExts.push_back({vk::KHRPortabilitySubsetExtensionName, false});
    // VK_EXT_memory_budget — used by VMA
    m_DeviceExts.push_back({vk::EXTMemoryBudgetExtensionName, false});
    // VK_EXT_calibrated_timestamps — used by GPU profiling
    m_DeviceExts.push_back({vk::EXTCalibratedTimestampsExtensionName, false});
    // VK_KHR_copy_commands2 — the frame-start upload batch records vkCmdCopyBuffer2
    m_DeviceExts.push_back({vk::KHRCopyCommands2ExtensionName, true});

    // Hardware ray tracing is an optional all-or-nothing device capability. These extensions are appended
    // only after ResolveRayTracingSupport validates the entire extension and feature bundle for the
    // selected physical device.
    m_RayTracingExts.push_back({vk::KHRAccelerationStructureExtensionName, false});
    m_RayTracingExts.push_back({vk::KHRRayTracingPipelineExtensionName, false});
    m_RayTracingExts.push_back({vk::KHRDeferredHostOperationsExtensionName, false});
}

} // namespace SoulEngine
