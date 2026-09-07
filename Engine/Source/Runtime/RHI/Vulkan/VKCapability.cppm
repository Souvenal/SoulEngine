export module Vulkan:Capability;

import Core;
import vulkan;
import std;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// Instance and device capability requests
// ═════════════════════════════════════════════════════════════════════════════

struct VulkanCapabilityRequest {
    const char* Name     = nullptr;
    bool        Required = false;
    bool        Enabled  = false; // set during resolve
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanCapability
// ═════════════════════════════════════════════════════════════════════════════

using VulkanFeaturesChain = vk::StructureChain<vk::PhysicalDeviceFeatures2,
                                               vk::PhysicalDeviceVulkan11Features,
                                               vk::PhysicalDeviceVulkan12Features,
                                               vk::PhysicalDeviceVulkan13Features,
                                               vk::PhysicalDeviceVulkan14Features,
                                               vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                               vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>;

using VulkanPropertiesChain = vk::StructureChain<vk::PhysicalDeviceProperties2,
                                                 vk::PhysicalDeviceVulkan11Properties,
                                                 vk::PhysicalDeviceVulkan12Properties,
                                                 vk::PhysicalDeviceVulkan13Properties,
                                                 vk::PhysicalDeviceVulkan14Properties,
                                                 vk::PhysicalDeviceAccelerationStructurePropertiesKHR,
                                                 vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>;

class VulkanCapability : public Singleton<VulkanCapability> {
    friend class Singleton<VulkanCapability>;

  private:
    auto AddRequiredRequests(std::vector<VulkanCapabilityRequest>& Requests, std::span<const char*> RequiredNames)
        -> void {
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

    template <typename PropertyType, typename GetName>
    [[nodiscard]] auto MatchRequests(std::vector<VulkanCapabilityRequest>& Requests,
                                     const std::vector<PropertyType>&      Available,
                                     GetName&&                             GetPropertyName,
                                     StringView RequestKind) -> std::expected<std::vector<const char*>, ErrorMessage> {
        for (auto& Request : Requests)
            if (Request.Name)
                Request.Enabled = false;

        for (auto& Request : Requests) {
            if (!Request.Name)
                continue;
            if (std::ranges::any_of(Available, [&](const auto& Property) {
                    return std::strcmp(GetPropertyName(Property), Request.Name) == 0;
                }))
                Request.Enabled = true;
        }

        std::vector<StringView> Missing;
        for (const auto& Request : Requests) {
            if (Request.Name && Request.Required && !Request.Enabled)
                Missing.emplace_back(Request.Name);
        }
        if (!Missing.empty()) {
            String Msg;
            for (std::size_t i = 0; i < Missing.size(); ++i) {
                if (i > 0)
                    Msg += ", ";
                Msg += Missing[i];
            }
            return std::unexpected(ErrorMessage(Format("Required Vulkan {} not supported: {}", RequestKind, Msg)));
        }

        std::vector<const char*> EnabledNames;
        for (const auto& Request : Requests) {
            if (Request.Name && Request.Enabled)
                EnabledNames.emplace_back(Request.Name);
        }
        return EnabledNames;
    }

  public:
    // ── Phase 1: Resolve instance extensions ─────────────────────────────

    [[nodiscard]] auto ResolveInstanceExtensions(vk::raii::Context& Ctx, std::span<const char*> RequiredExtensions)
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

    [[nodiscard]] auto ResolveInstanceLayers(vk::raii::Context& Ctx)
        -> std::expected<std::vector<const char*>, ErrorMessage> {
        std::vector<VulkanCapabilityRequest> InstanceLayers;
        const bool Validation = ConfigManager::Get().GetConfig().RhiVulkan.Validation.value_or(true);
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

    // ── Phase 2: Resolve device extensions + query features ──────────────

    [[nodiscard]] auto ResolveDeviceExtensionsAndFeatures(vk::raii::PhysicalDevice& PD)
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

        // Add ray tracing extensions if they are available
        for (const auto& Ext : m_RayTracingExts) {
            if (Ext.Enabled)
                m_EnabledDeviceNames.emplace_back(Ext.Name);
        }

        return std::tuple<std::vector<const char*>, const VulkanFeaturesChain&>{m_EnabledDeviceNames,
                                                                                m_SupportedFeatures};
    }

    // ── Phase 3: Query device properties ────────────────────────────────

    // ── Phase 3: Query device properties ──────────────────────

    /// Fill the properties chain via vkGetPhysicalDeviceProperties2.  Call
    /// after physical-device selection and before device creation.
    auto ResolveDeviceProperties(vk::raii::PhysicalDevice& PD) -> void {
        m_Properties = PD.getProperties2<vk::PhysicalDeviceProperties2,
                                         vk::PhysicalDeviceVulkan11Properties,
                                         vk::PhysicalDeviceVulkan12Properties,
                                         vk::PhysicalDeviceVulkan13Properties,
                                         vk::PhysicalDeviceVulkan14Properties,
                                         vk::PhysicalDeviceAccelerationStructurePropertiesKHR,
                                         vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    }

    // ── Queries ──────────────────────────────────────────────────────────

    /// Core features (Vulkan 1.0).
    [[nodiscard]] auto GetFeatures() const -> const vk::PhysicalDeviceFeatures& {
        return m_SupportedFeatures.get<vk::PhysicalDeviceFeatures2>().features;
    }

    /// Extension features (Vulkan 1.1+).
    template <typename T>
    [[nodiscard]] auto GetFeatures() const -> const T& {
        return m_SupportedFeatures.get<T>();
    }

    /// Core properties (Vulkan 1.0).
    [[nodiscard]] auto GetProperties() const -> const vk::PhysicalDeviceProperties& {
        return m_Properties.get<vk::PhysicalDeviceProperties2>().properties;
    }

    /// Extension properties (Vulkan 1.1+).
    template <typename T>
    [[nodiscard]] auto GetProperties() const -> const T& {
        return m_Properties.get<T>();
    }

    [[nodiscard]] auto IsInstanceExtensionEnabled(const char* Name) -> bool {
        return std::ranges::any_of(m_EnabledInstanceExtensionNames,
                                   [Name](const auto* EnabledName) { return std::strcmp(EnabledName, Name) == 0; });
    }

    [[nodiscard]] auto IsDeviceExtensionEnabled(const char* Name) -> bool {
        const auto IsEnabled = [Name](const std::vector<VulkanCapabilityRequest>& Extensions) -> bool {
            auto It = std::ranges::find_if(Extensions,
                                           [Name](const auto& E) { return E.Name && std::strcmp(E.Name, Name) == 0; });
            return It != Extensions.end() && It->Enabled;
        };
        return IsEnabled(m_DeviceExts) || IsEnabled(m_RayTracingExts);
    }

    [[nodiscard]] auto IsRayTracingAvailable() -> bool {
        return IsDeviceExtensionEnabled(vk::KHRAccelerationStructureExtensionName);
    }

  private:
    VulkanCapability() {
        RegisterInstanceExtensions();
        RegisterDeviceExtensions();
    }

    // ── Registration ────────────────────────────────────────────────────

    auto RegisterInstanceExtensions() -> void {
        m_InstanceExts.emplace_back(vk::KHRPortabilityEnumerationExtensionName, false);
    }

    auto RegisterDeviceExtensions() -> void {
        // VK_KHR_swapchain — required for presenting
        m_DeviceExts.push_back({vk::KHRSwapchainExtensionName, true});
        // VK_KHR_portability_subset — needed for MoltenVK
        m_DeviceExts.push_back({vk::KHRPortabilitySubsetExtensionName, false});
        // VK_EXT_memory_budget — used by VMA
        m_DeviceExts.push_back({vk::EXTMemoryBudgetExtensionName, false});

        // Hardware ray tracing is an optional all-or-nothing device capability.
        // These extensions are appended only after ResolveRayTracingSupport validates
        // the entire extension and feature bundle for the selected physical device.
        m_RayTracingExts.push_back({vk::KHRAccelerationStructureExtensionName, false});
        m_RayTracingExts.push_back({vk::KHRRayTracingPipelineExtensionName, false});
        m_RayTracingExts.push_back({vk::KHRDeferredHostOperationsExtensionName, false});
    }

    // ── Members ─────────────────────────────────────────────────────────

    std::vector<VulkanCapabilityRequest> m_InstanceExts;
    std::vector<VulkanCapabilityRequest> m_DeviceExts;
    std::vector<VulkanCapabilityRequest> m_RayTracingExts;
    std::vector<const char*>             m_EnabledInstanceExtensionNames;
    std::vector<const char*>             m_EnabledDeviceNames;

    VulkanFeaturesChain   m_SupportedFeatures; ///< Queried from physical device
    VulkanPropertiesChain m_Properties;        ///< Queried from physical device
};

} // namespace SoulEngine
