module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module Vulkan:Capability;

import Core;
import vulkan;
import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// Extension request
// ═════════════════════════════════════════════════════════════════════════════

struct ExtensionRequest {
    const char* Name     = nullptr;
    bool        Required = false;
    bool        Enabled  = false; // set during resolve
};

// ═════════════════════════════════════════════════════════════════════════════
// Capability
// ═════════════════════════════════════════════════════════════════════════════

using FeaturesChain = vk::StructureChain<vk::PhysicalDeviceFeatures2,
                                         vk::PhysicalDeviceVulkan11Features,
                                         vk::PhysicalDeviceVulkan12Features,
                                         vk::PhysicalDeviceVulkan13Features,
                                         vk::PhysicalDeviceVulkan14Features,
                                         vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                         vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>;

using PropertiesChain = vk::StructureChain<vk::PhysicalDeviceProperties2,
                                           vk::PhysicalDeviceVulkan11Properties,
                                           vk::PhysicalDeviceVulkan12Properties,
                                           vk::PhysicalDeviceVulkan13Properties,
                                           vk::PhysicalDeviceVulkan14Properties,
                                           vk::PhysicalDeviceAccelerationStructurePropertiesKHR,
                                           vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>;

struct RayTracingSupport {
    bool   Available         = false;
    String UnavailableReason = {};
};

class Capability : public Singleton<Capability> {
    friend class Singleton<Capability>;

  public:
    // ── Phase 1: Resolve instance extensions ─────────────────────────────

    [[nodiscard]] auto ResolveInstanceExtensions(vk::raii::Context& Ctx)
        -> std::expected<std::vector<const char*>, ErrorMessage> {
        // Merge GLFW-required extensions — all are required.
        uint32_t GlfwCount = 0;
        auto*    GlfwExts  = glfwGetRequiredInstanceExtensions(&GlfwCount);
        if (!GlfwExts) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(
                ErrorMessage(Core::Format("glfwGetRequiredInstanceExtensions failed: {}",
                                          Desc ? Desc : "GLFW not initialized or no Vulkan support")));
        }
        for (auto* GlfwExt : std::span(GlfwExts, GlfwCount)) {
            auto It = std::ranges::find_if(m_InstanceExts,
                                           [&](const auto& E) { return E.Name && std::strcmp(E.Name, GlfwExt) == 0; });
            if (It != m_InstanceExts.end())
                It->Enabled = false; // re-enabled below if found in driver
            else
                m_InstanceExts.emplace_back(GlfwExt, true, false);
        }

        // Enumerate available instance extensions
        auto ExtPropsRes = Ctx.enumerateInstanceExtensionProperties();
        if (ExtPropsRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to enumerate Vulkan instance extension properties"));
        auto& ExtProps = ExtPropsRes.value;

        for (auto& Prop : ExtProps)
            LogDebug("Available Vulkan instance extension: {}", static_cast<const char*>(Prop.extensionName));

        return MatchExtensions(m_InstanceExts, ExtProps);
    }

    // ── Phase 2: Resolve device extensions + query features ──────────────

    [[nodiscard]] auto ResolveDeviceExtensionsAndFeatures(vk::raii::PhysicalDevice& PD)
        -> std::expected<std::tuple<std::vector<const char*>, const FeaturesChain&>, ErrorMessage> {
        // Enumerate available device extensions
        auto ExtPropsRes = PD.enumerateDeviceExtensionProperties();
        if (ExtPropsRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to enumerate device extension properties"));
        auto& ExtProps = ExtPropsRes.value;
        for (auto& Prop : ExtProps)
            LogDebug("Available device extension: {}", static_cast<const char*>(Prop.extensionName));

        // Match extensions
        auto Result = MatchExtensions(m_DeviceExts, ExtProps);
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

        ResolveRayTracingSupport(ExtProps, PD.getProperties().apiVersion);
        if (m_RayTracingSupport.Available) {
            for (const auto& Ext : m_RayTracingExts)
                m_EnabledDeviceNames.emplace_back(Ext.Name);
        } else {
            // These extension feature structs are included in the common query chain.
            // Do not request them from a device unless the full extension/feature bundle is enabled.
            m_SupportedFeatures.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure = false;
            m_SupportedFeatures.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline = false;
        }

        return std::tuple<std::vector<const char*>, const FeaturesChain&>{m_EnabledDeviceNames, m_SupportedFeatures};
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
        auto It = std::ranges::find_if(m_InstanceExts,
                                       [&](const auto& E) { return E.Name && std::strcmp(E.Name, Name) == 0; });
        return It != m_InstanceExts.end() && It->Enabled;
    }

    [[nodiscard]] auto IsDeviceExtensionEnabled(const char* Name) -> bool {
        const auto IsEnabled = [Name](const std::vector<ExtensionRequest>& Extensions) -> bool {
            auto It = std::ranges::find_if(
                Extensions, [Name](const auto& E) { return E.Name && std::strcmp(E.Name, Name) == 0; });
            return It != Extensions.end() && It->Enabled;
        };
        return IsEnabled(m_DeviceExts) || (m_RayTracingSupport.Available && IsEnabled(m_RayTracingExts));
    }

    [[nodiscard]] auto GetRayTracingSupport() const -> const RayTracingSupport& {
        return m_RayTracingSupport;
    }

  private:
    // ── Shared match logic ───────────────────────────────────────────────

    [[nodiscard]] auto MatchExtensions(std::vector<ExtensionRequest>&           Exts,
                                       std::span<const vk::ExtensionProperties> Available)
        -> std::expected<std::vector<const char*>, ErrorMessage> {
        for (auto& E : Exts)
            if (E.Name)
                E.Enabled = false;

        for (auto& E : Exts) {
            if (!E.Name)
                continue;
            if (std::ranges::any_of(Available,
                                    [&](const auto& P) { return std::strcmp(P.extensionName, E.Name) == 0; }))
                E.Enabled = true;
        }

        std::vector<StringView> Missing;
        for (const auto& E : Exts) {
            if (E.Name && E.Required && !E.Enabled)
                Missing.emplace_back(E.Name);
        }
        if (!Missing.empty()) {
            String Msg;
            for (size_t i = 0; i < Missing.size(); ++i) {
                if (i > 0)
                    Msg += ", ";
                Msg += Missing[i];
            }
            return std::unexpected(ErrorMessage(Core::Format("Required Vulkan extensions not supported: {}", Msg)));
        }

        std::vector<const char*> EnabledNames;
        for (const auto& E : Exts) {
            if (E.Name && E.Enabled)
                EnabledNames.emplace_back(E.Name);
        }
        return EnabledNames;
    }

  private:
    Capability() {
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
        // VK_KHR_buffer_device_address is promoted to core Vulkan 1.2. Keep
        // the extension enabled when a pre-1.2 RT device exposes it so the
        // Vulkan-Hpp dispatcher can resolve vkGetBufferDeviceAddress.
        m_DeviceExts.push_back({vk::KHRBufferDeviceAddressExtensionName, false});

        // Hardware ray tracing is an optional all-or-nothing device capability.
        // These extensions are appended only after ResolveRayTracingSupport validates
        // the entire extension and feature bundle for the selected physical device.
        m_RayTracingExts.push_back({vk::KHRAccelerationStructureExtensionName, false});
        m_RayTracingExts.push_back({vk::KHRRayTracingPipelineExtensionName, false});
        m_RayTracingExts.push_back({vk::KHRDeferredHostOperationsExtensionName, false});
    }

    auto ResolveRayTracingSupport(std::span<const vk::ExtensionProperties> AvailableExtensions, Uint32 ApiVersion) -> void {
        m_RayTracingSupport = {};
        const auto HasExtension = [AvailableExtensions](const char* Name) -> bool {
            return std::ranges::any_of(
                AvailableExtensions, [Name](const auto& Property) { return std::strcmp(Property.extensionName, Name) == 0; });
        };
        for (auto& Ext : m_RayTracingExts) {
            Ext.Enabled = HasExtension(Ext.Name);
            if (!Ext.Enabled) {
                m_RayTracingSupport.UnavailableReason = Format("Missing required ray-tracing device extension '{}'", Ext.Name);
                return;
            }
        }

        if (ApiVersion < VK_API_VERSION_1_2 && !HasExtension(vk::KHRBufferDeviceAddressExtensionName)) {
            m_RayTracingSupport.UnavailableReason = "VK_KHR_buffer_device_address is required before Vulkan 1.2";
            return;
        }
        if (ApiVersion < VK_API_VERSION_1_2 && !HasExtension(vk::KHRSpirv14ExtensionName)) {
            m_RayTracingSupport.UnavailableReason = "VK_KHR_spirv_1_4 is required before Vulkan 1.2";
            return;
        }
        if (ApiVersion < VK_API_VERSION_1_2 && !HasExtension(vk::KHRShaderFloatControlsExtensionName)) {
            m_RayTracingSupport.UnavailableReason = "VK_KHR_shader_float_controls is required before Vulkan 1.2";
            return;
        }

        const auto& V12 = m_SupportedFeatures.get<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.bufferDeviceAddress) {
            m_RayTracingSupport.UnavailableReason = "bufferDeviceAddress feature is not supported";
            return;
        }
        if (!m_SupportedFeatures.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure) {
            m_RayTracingSupport.UnavailableReason = "accelerationStructure feature is not supported";
            return;
        }
        if (!m_SupportedFeatures.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline) {
            m_RayTracingSupport.UnavailableReason = "rayTracingPipeline feature is not supported";
            return;
        }

        m_RayTracingSupport.Available = true;
    }

    // ── Members ─────────────────────────────────────────────────────────

    std::vector<ExtensionRequest> m_InstanceExts;
    std::vector<ExtensionRequest> m_DeviceExts;
    std::vector<ExtensionRequest> m_RayTracingExts;
    std::vector<const char*>      m_EnabledDeviceNames;
    RayTracingSupport             m_RayTracingSupport = {};

    FeaturesChain   m_SupportedFeatures; ///< Queried from physical device
    PropertiesChain m_Properties;        ///< Queried from physical device
};

} // namespace SoulEngine::RHI::Vulkan
