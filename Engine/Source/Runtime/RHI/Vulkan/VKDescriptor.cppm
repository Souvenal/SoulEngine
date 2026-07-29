module;

export module Vulkan:Descriptor;

import Core;
import vulkan;
import std;

import :Capability;
import :Buffer;

namespace SoulEngine {

auto VulkanWriteConstantArenaDescriptor(vk::raii::Device& Device,
                                  vk::Buffer Buffer,
                                  vk::DescriptorSet Set,
                                  Uint32 Binding,
                                  Uint64 Range) -> void {
    vk::DescriptorBufferInfo BufInfo{
        .buffer = Buffer,
        .offset = 0,
        .range  = Range,
    };
    vk::WriteDescriptorSet Write{
        .dstSet          = Set,
        .dstBinding      = Binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType  = vk::DescriptorType::eUniformBufferDynamic,
        .pBufferInfo     = &BufInfo,
    };
    Device.updateDescriptorSets(Write, {});
}

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDescriptorManager
// ═════════════════════════════════════════════════════════════════════════════

/// Owns descriptor allocation and draw-scope descriptor writes.
/// RHIPipeline-specific descriptor set layouts and pipeline layouts are created
/// from shader reflection by VulkanGraphicsPipeline.
class VulkanDescriptorManager {
  public:
    VulkanDescriptorManager() = default;

    VulkanDescriptorManager(VulkanDescriptorManager&&) noexcept                    = default;
    auto operator=(VulkanDescriptorManager&&) noexcept -> VulkanDescriptorManager& = default;

    VulkanDescriptorManager(const VulkanDescriptorManager&)                    = delete;
    auto operator=(const VulkanDescriptorManager&) -> VulkanDescriptorManager& = delete;

    /// @param Device           Vulkan device handle.
    /// @param FramesInFlight   Number of frame slots.
    [[nodiscard]] static auto Create(vk::raii::Device& Device, Uint32 FramesInFlight)
        -> std::expected<VulkanDescriptorManager, ErrorMessage> {
        // ── Verify descriptor indexing features are supported ───────────
        const auto& V12 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound ||
            !V12.descriptorBindingVariableDescriptorCount || !V12.runtimeDescriptorArray ||
            !V12.descriptorBindingSampledImageUpdateAfterBind)
            return std::unexpected(
                ErrorMessage("VulkanDescriptorManager: required Vulkan 1.2 descriptor indexing features not supported by device"));

        VulkanDescriptorManager Mgr;
        Mgr.m_Device         = &Device;
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
        if (VulkanCapability::Get().GetRayTracingSupport().Available)
            PoolSizes.emplace_back(vk::DescriptorType::eAccelerationStructureKHR, ScratchDescriptorCount);
        Uint32 MaxSets = SharedDescriptorCount + 1;
        vk::DescriptorPoolCreateInfo PoolCI{
            // eFreeDescriptorSet is required because scratch sets are
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
        Mgr.m_Pool = std::move(PoolRes.value);

        Mgr.m_ScratchSets.resize(FramesInFlight);
        return Mgr;
    }

    // ── Binding ─────────────────────────────────────────────────────────

    auto BeginFrame(Uint32 FrameIndex) -> void {
        if (FrameIndex < m_ScratchSets.size())
            m_ScratchSets[FrameIndex].clear();
    }

    [[nodiscard]] auto GetDescriptorPool() const -> vk::DescriptorPool {
        return *m_Pool;
    }

    [[nodiscard]] auto AllocateDescriptorSets(Uint32 FrameIndex, std::span<const vk::DescriptorSetLayout> SetLayouts)
        -> std::expected<std::vector<vk::DescriptorSet>, ErrorMessage> {
        std::vector<Uint32> VariableCounts(SetLayouts.size(), 1);
        return AllocateDescriptorSets(FrameIndex, SetLayouts, VariableCounts);
    }

    [[nodiscard]] auto AllocateDescriptorSets(Uint32 FrameIndex,
                                              std::span<const vk::DescriptorSetLayout> SetLayouts,
                                              std::span<const Uint32> VariableCounts)
        -> std::expected<std::vector<vk::DescriptorSet>, ErrorMessage> {
        if (SetLayouts.empty())
            return std::vector<vk::DescriptorSet>{};
        if (FrameIndex >= m_ScratchSets.size())
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: invalid frame index for scratch descriptor sets"));
        if (VariableCounts.size() != SetLayouts.size())
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: variable descriptor counts do not match set layouts"));

        vk::DescriptorSetVariableDescriptorCountAllocateInfo VariableInfo{
            .descriptorSetCount = static_cast<Uint32>(VariableCounts.size()),
            .pDescriptorCounts  = VariableCounts.data(),
        };
        vk::StructureChain<vk::DescriptorSetAllocateInfo, vk::DescriptorSetVariableDescriptorCountAllocateInfo>
             AllocChain = {
                 {.descriptorPool     = *m_Pool,
                  .descriptorSetCount = static_cast<Uint32>(SetLayouts.size()),
                  .pSetLayouts        = SetLayouts.data()},
                 VariableInfo,
             };
        auto Res = m_Device->allocateDescriptorSets(AllocChain.get<vk::DescriptorSetAllocateInfo>());
        if (Res.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to allocate draw descriptor sets"));

        std::vector<vk::DescriptorSet> RawSets;
        RawSets.reserve(Res.value.size());
        auto& Scratch = m_ScratchSets[FrameIndex];
        Scratch.reserve(Scratch.size() + Res.value.size());
        for (auto& Set : Res.value) {
            RawSets.push_back(*Set);
            Scratch.push_back(std::move(Set));
        }
        return RawSets;
    }

    [[nodiscard]] auto AllocateDescriptorSet(Uint32 FrameIndex,
                                             vk::DescriptorSetLayout SetLayout,
                                             Uint32 VariableDescriptorCount)
        -> std::expected<vk::DescriptorSet, ErrorMessage>;

    [[nodiscard]] auto AllocatePersistentDescriptorSet(vk::DescriptorSetLayout SetLayout,
                                                        Uint32                  VariableDescriptorCount)
        -> std::expected<vk::raii::DescriptorSet, ErrorMessage>;

    auto WriteConstantDescriptor(vk::DescriptorSet Set, Uint32 Binding, vk::Buffer Buffer, Uint64 Range) -> void {
        VulkanWriteConstantArenaDescriptor(*m_Device, Buffer, Set, Binding, Range);
    }

    auto WriteStorageBufferDescriptor(vk::DescriptorSet Set,
                                      Uint32            Binding,
                                      vk::Buffer        Buffer,
                                      Uint64            Range,
                                      Uint64            Offset = 0) -> void {
        vk::DescriptorBufferInfo BufferInfo{.buffer = Buffer, .offset = Offset, .range = Range};
        vk::WriteDescriptorSet Write{
            .dstSet = Set, .dstBinding = Binding, .dstArrayElement = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &BufferInfo};
        m_Device->updateDescriptorSets(Write, {});
    }

    auto WriteSampledTextureDescriptor(vk::DescriptorSet Set,
                                       Uint32            Binding,
                                       Uint32            ArrayElement,
                                       vk::ImageView     ImageView,
                                       vk::ImageLayout   Layout) -> void {
        vk::DescriptorImageInfo ImageInfo{
            .sampler     = nullptr,
            .imageView   = ImageView,
            .imageLayout = Layout,
        };
        vk::WriteDescriptorSet Write{
            .dstSet          = Set,
            .dstBinding      = Binding,
            .dstArrayElement = ArrayElement,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampledImage,
            .pImageInfo      = &ImageInfo,
        };
        m_Device->updateDescriptorSets(Write, {});
    }

    auto WriteStorageImageDescriptor(vk::DescriptorSet Set, Uint32 Binding, vk::ImageView ImageView) -> void {
        vk::DescriptorImageInfo ImageInfo{
            .sampler     = nullptr,
            .imageView   = ImageView,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        vk::WriteDescriptorSet Write{
            .dstSet          = Set,
            .dstBinding      = Binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eStorageImage,
            .pImageInfo      = &ImageInfo,
        };
        m_Device->updateDescriptorSets(Write, {});
    }

    auto WriteAccelerationStructureDescriptor(vk::DescriptorSet Set,
                                              Uint32            Binding,
                                              vk::AccelerationStructureKHR RHIAccelerationStructure) -> void {
        vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR> WriteChain = {
            {.dstSet          = Set,
             .dstBinding      = Binding,
             .dstArrayElement = 0,
             .descriptorCount = 1,
             .descriptorType  = vk::DescriptorType::eAccelerationStructureKHR},
            {.accelerationStructureCount = 1, .pAccelerationStructures = &RHIAccelerationStructure},
        };
        m_Device->updateDescriptorSets(WriteChain.get<vk::WriteDescriptorSet>(), {});
    }

    auto WriteSamplerDescriptor(vk::DescriptorSet Set, Uint32 Binding, vk::Sampler VulkanSampler) -> void {
        vk::DescriptorImageInfo ImageInfo{
            .sampler     = VulkanSampler,
            .imageView   = nullptr,
            .imageLayout = vk::ImageLayout::eUndefined,
        };
        vk::WriteDescriptorSet Write{
            .dstSet          = Set,
            .dstBinding      = Binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampler,
            .pImageInfo      = &ImageInfo,
        };
        m_Device->updateDescriptorSets(Write, {});
    }

    // ── Members ─────────────────────────────────────────────────────────

    vk::raii::Device* m_Device         = nullptr;
    Uint32            m_FramesInFlight = 2;

    vk::raii::DescriptorPool                          m_Pool = nullptr;
    std::vector<std::vector<vk::raii::DescriptorSet>> m_ScratchSets;
};

auto VulkanDescriptorManager::AllocateDescriptorSet(Uint32                  FrameIndex,
                                              vk::DescriptorSetLayout SetLayout,
                                              Uint32                  VariableDescriptorCount)
    -> std::expected<vk::DescriptorSet, ErrorMessage> {
    std::array Layouts{SetLayout};
    std::array Counts{VariableDescriptorCount};
    auto Sets = AllocateDescriptorSets(FrameIndex,
                                       std::span<const vk::DescriptorSetLayout>(Layouts),
                                       std::span<const Uint32>(Counts));
    if (!Sets)
        return std::unexpected(Sets.error());
    if (Sets->empty())
        return std::unexpected(ErrorMessage("VulkanDescriptorManager: single descriptor set allocation returned no sets"));
    return (*Sets)[0];
}

auto VulkanDescriptorManager::AllocatePersistentDescriptorSet(vk::DescriptorSetLayout SetLayout,
                                                         Uint32                  VariableDescriptorCount)
    -> std::expected<vk::raii::DescriptorSet, ErrorMessage> {
    std::array Layouts{SetLayout};
    std::array Counts{VariableDescriptorCount};
    vk::DescriptorSetVariableDescriptorCountAllocateInfo VariableInfo{
        .descriptorSetCount = static_cast<Uint32>(Counts.size()),
        .pDescriptorCounts  = Counts.data(),
    };
    vk::StructureChain<vk::DescriptorSetAllocateInfo, vk::DescriptorSetVariableDescriptorCountAllocateInfo>
         AllocChain = {
             {.descriptorPool     = *m_Pool,
              .descriptorSetCount = static_cast<Uint32>(Layouts.size()),
              .pSetLayouts        = Layouts.data()},
             VariableInfo,
         };
    auto Res = m_Device->allocateDescriptorSets(AllocChain.get<vk::DescriptorSetAllocateInfo>());
    if (Res.result != vk::Result::eSuccess || Res.value.empty())
        return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to allocate persistent descriptor set"));
    return std::move(Res.value.front());
}

} // namespace SoulEngine
