export module Vulkan:Capability;

import Core;
import vulkan;
import std;

namespace SoulEngine {

// Instance and device capability requests

struct VulkanCapabilityRequest {
    const char* Name     = nullptr;
    bool        Required = false;
    bool        Enabled  = false; // set during resolve
};

// VulkanCapability

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
        -> void;

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

    // Phase 1: Resolve instance extensions
  public:
    [[nodiscard]] auto ResolveInstanceExtensions(vk::raii::Context& Ctx, std::span<const char*> RequiredExtensions)
        -> std::expected<std::vector<const char*>, ErrorMessage>;

    [[nodiscard]] auto ResolveInstanceLayers(vk::raii::Context& Ctx)
        -> std::expected<std::vector<const char*>, ErrorMessage>;
    // Phase 2: Resolve device extensions + query features

    [[nodiscard]] auto ResolveDeviceExtensionsAndFeatures(vk::raii::PhysicalDevice& PD)
        -> std::expected<std::tuple<std::vector<const char*>, const VulkanFeaturesChain&>, ErrorMessage>;
    // Phase 3: Query device properties

    /// @brief Fill the properties chain via vkGetPhysicalDeviceProperties2.
    ///
    /// Call after physical-device selection and before device creation.
    auto ResolveDeviceProperties(vk::raii::PhysicalDevice& PD) -> void;
    // Queries

    /// Core features (Vulkan 1.0).
    [[nodiscard]] auto GetFeatures() const -> const vk::PhysicalDeviceFeatures&;

    /// Extension features (Vulkan 1.1+).
    template <typename T>
    [[nodiscard]] auto GetFeatures() const -> const T& {
        return m_SupportedFeatures.get<T>();
    }

    /// Core properties (Vulkan 1.0).
    [[nodiscard]] auto GetProperties() const -> const vk::PhysicalDeviceProperties&;

    /// Extension properties (Vulkan 1.1+).
    template <typename T>
    [[nodiscard]] auto GetProperties() const -> const T& {
        return m_Properties.get<T>();
    }

    [[nodiscard]] auto IsInstanceExtensionEnabled(const char* Name) -> bool;

    [[nodiscard]] auto IsDeviceExtensionEnabled(const char* Name) -> bool;

    [[nodiscard]] auto IsRayTracingAvailable() -> bool;

  private:
    VulkanCapability();
    // Registration

    auto RegisterInstanceExtensions() -> void;

    auto RegisterDeviceExtensions() -> void;
    // Members

    std::vector<VulkanCapabilityRequest> m_InstanceExts;
    std::vector<VulkanCapabilityRequest> m_DeviceExts;
    std::vector<VulkanCapabilityRequest> m_RayTracingExts;
    std::vector<const char*>             m_EnabledInstanceExtensionNames;
    std::vector<const char*>             m_EnabledDeviceNames;

    VulkanFeaturesChain   m_SupportedFeatures; ///< Queried from physical device
    VulkanPropertiesChain m_Properties;        ///< Queried from physical device
};

} // namespace SoulEngine
