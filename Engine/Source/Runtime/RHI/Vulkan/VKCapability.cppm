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
                                         vk::PhysicalDeviceVulkan14Features>;

using PropertiesChain = vk::StructureChain<vk::PhysicalDeviceProperties2,
                                           vk::PhysicalDeviceVulkan11Properties,
                                           vk::PhysicalDeviceVulkan12Properties,
                                           vk::PhysicalDeviceVulkan13Properties,
                                           vk::PhysicalDeviceVulkan14Properties>;

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

        // Query supported features from physical device
        m_SupportedFeatures = PD.getFeatures2<vk::PhysicalDeviceFeatures2,
                                              vk::PhysicalDeviceVulkan11Features,
                                              vk::PhysicalDeviceVulkan12Features,
                                              vk::PhysicalDeviceVulkan13Features,
                                              vk::PhysicalDeviceVulkan14Features>();

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
                                         vk::PhysicalDeviceVulkan14Properties>();
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
        auto It =
            std::ranges::find_if(m_DeviceExts, [&](const auto& E) { return E.Name && std::strcmp(E.Name, Name) == 0; });
        return It != m_DeviceExts.end() && It->Enabled;
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
    }

    // ── Members ─────────────────────────────────────────────────────────

    std::vector<ExtensionRequest> m_InstanceExts;
    std::vector<ExtensionRequest> m_DeviceExts;
    std::vector<const char*>      m_EnabledDeviceNames;

    FeaturesChain   m_SupportedFeatures; ///< Queried from physical device
    PropertiesChain m_Properties;        ///< Queried from physical device
};

} // namespace SoulEngine::RHI::Vulkan
