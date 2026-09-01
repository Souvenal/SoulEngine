module;

export module Vulkan:Descriptor;

import Core;
import vulkan;
import std;

import :Capability;
import :Debug;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDescriptorManager
// ═════════════════════════════════════════════════════════════════════════════

/// Owns the global descriptor pool, persistent descriptor set allocation, and
/// descriptor write primitives. Descriptor set layouts and pipeline layouts are
/// created from shader reflection by VulkanShaderBindingSet.
class VulkanDescriptorManager {
  public:
    VulkanDescriptorManager() = default;

    // Custom moves: m_PoolMutex is not movable.
    VulkanDescriptorManager(VulkanDescriptorManager&& Other) noexcept
        : m_Device(Other.m_Device),
          m_DebugUtils(Other.m_DebugUtils),
          m_FramesInFlight(Other.m_FramesInFlight),
          m_NextPersistentSet(Other.m_NextPersistentSet),
          m_Pool(std::move(Other.m_Pool)) {}
    auto operator=(VulkanDescriptorManager&& Other) noexcept -> VulkanDescriptorManager& {
        m_Device             = Other.m_Device;
        m_DebugUtils         = Other.m_DebugUtils;
        m_FramesInFlight     = Other.m_FramesInFlight;
        m_NextPersistentSet  = Other.m_NextPersistentSet;
        m_Pool               = std::move(Other.m_Pool);
        return *this;
    }

    VulkanDescriptorManager(const VulkanDescriptorManager&)                    = delete;
    auto operator=(const VulkanDescriptorManager&) -> VulkanDescriptorManager& = delete;

    /// @param Device           Vulkan device handle.
    /// @param FramesInFlight   Number of frame slots.
    [[nodiscard]] static auto Create(vk::raii::Device& Device, VulkanDebugUtils& DebugUtils, Uint32 FramesInFlight)
        -> std::expected<VulkanDescriptorManager, ErrorMessage> {
        // ── Verify descriptor indexing features are supported ───────────
        const auto& V12 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound ||
            !V12.runtimeDescriptorArray ||
            !V12.descriptorBindingSampledImageUpdateAfterBind)
            return std::unexpected(
                ErrorMessage("VulkanDescriptorManager: required Vulkan 1.2 descriptor indexing features not supported by device"));

        VulkanDescriptorManager Mgr;
        Mgr.m_Device         = &Device;
        Mgr.m_DebugUtils     = &DebugUtils;
        Mgr.m_FramesInFlight = FramesInFlight;

        // ── Descriptor pool ─────────────────────────────────────────────
        constexpr Uint32 ScratchDescriptorCount           = 4096;
        constexpr Uint32 ImGuiDescriptorCount             = 4096;
        constexpr Uint32 SharedDescriptorCount            = ScratchDescriptorCount + ImGuiDescriptorCount;
        constexpr Uint32 PersistentSampledImageSetBudget = 4;
        constexpr Uint32 SampledImageDescriptorCount =
            ScratchDescriptorCount * (PersistentSampledImageSetBudget + 1) + ImGuiDescriptorCount;
        std::vector<vk::DescriptorPoolSize> PoolSizes{
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, SampledImageDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformTexelBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageTexelBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBufferDynamic, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBufferDynamic, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eInputAttachment, ImGuiDescriptorCount},
        };
        if (VulkanCapability::Get().IsRayTracingAvailable())
            PoolSizes.emplace_back(vk::DescriptorType::eAccelerationStructureKHR, ScratchDescriptorCount);
        Uint32 MaxSets = SharedDescriptorCount + 1;
        vk::DescriptorPoolCreateInfo PoolCI{
            // eFreeDescriptorSet is required because allocated sets are
            // vk::raii::DescriptorSet, whose destructors call vkFreeDescriptorSets.
            .flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                             vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets       = MaxSets,
            .poolSizeCount = static_cast<Uint32>(PoolSizes.size()),
            .pPoolSizes    = PoolSizes.data(),
        };
        auto PoolRes = Device.createDescriptorPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to create descriptor pool"));
        DebugUtils.SetObjectName(*PoolRes.value, "Internal/DescriptorPool/Global");
        Mgr.m_Pool = std::move(PoolRes.value);
        return Mgr;
    }

    // ── Binding ─────────────────────────────────────────────────────────

    [[nodiscard]] auto GetDescriptorPool() const -> vk::DescriptorPool {
        return *m_Pool;
    }

    [[nodiscard]] auto GetFramesInFlight() const -> Uint32 {
        return m_FramesInFlight;
    }

    [[nodiscard]] auto AllocateDescriptorSets(std::span<const vk::DescriptorSetLayout> Layouts)
        -> std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage> {
        if (Layouts.empty())
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: descriptor set layout list is empty"));

        // Pool allocation requires external synchronization; binding sets may be
        // created on the render thread (per-view sets) while the RHI thread
        // allocates for pipeline binding sets.
        std::scoped_lock Lock(m_PoolMutex);
        vk::DescriptorSetAllocateInfo AllocateInfo{
            .descriptorPool     = *m_Pool,
            .descriptorSetCount = static_cast<Uint32>(Layouts.size()),
            .pSetLayouts        = Layouts.data(),
        };
        auto Res = m_Device->allocateDescriptorSets(AllocateInfo);
        if (Res.result != vk::Result::eSuccess || Res.value.size() != Layouts.size())
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to allocate descriptor sets"));

        std::vector<vk::raii::DescriptorSet> Sets;
        Sets.reserve(Res.value.size());
        for (auto& Set : Res.value) {
            if (m_DebugUtils)
                m_DebugUtils->SetObjectName(
                    *Set, Format("Internal/DescriptorSet/Persistent/Set{}", m_NextPersistentSet++));
            Sets.push_back(std::move(Set));
        }
        return Sets;
    }

    auto WriteDescriptorSets(std::span<const vk::WriteDescriptorSet> Writes) -> void {
        if (!Writes.empty())
            m_Device->updateDescriptorSets(Writes, {});
    }

    // ── Members ─────────────────────────────────────────────────────────

    vk::raii::Device* m_Device              = nullptr;
    VulkanDebugUtils* m_DebugUtils          = nullptr;
    Uint32            m_FramesInFlight      = 2;
    Uint32            m_NextPersistentSet   = 0;

    vk::raii::DescriptorPool m_Pool = nullptr;
    std::mutex               m_PoolMutex = {};
};

} // namespace SoulEngine
