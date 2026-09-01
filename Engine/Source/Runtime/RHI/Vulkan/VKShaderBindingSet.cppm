module;

export module Vulkan:ShaderBindingSet;

import RHI;
import Shader;
import vulkan;
import std;

import :AccelerationStructure;
import :Buffer;
import :Capability;
import :Context;
import :Debug;
import :Descriptor;
import :Sampler;
import :Texture;

namespace SoulEngine {

namespace {

[[nodiscard]] auto ToVkDescriptorType(ShaderResourceType Type)
    -> std::expected<vk::DescriptorType, ErrorMessage> {
    switch (Type) {
    case ShaderResourceType::ConstantBuffer:
        return vk::DescriptorType::eUniformBufferDynamic;
    case ShaderResourceType::StorageBuffer:
        // Descriptor-bound storage buffers are always transient arena slices;
        // persistent buffers use vertex input or device addresses instead.
        return vk::DescriptorType::eStorageBufferDynamic;
    case ShaderResourceType::SampledTexture:
        return vk::DescriptorType::eSampledImage;
    case ShaderResourceType::StorageTexture:
        return vk::DescriptorType::eStorageImage;
    case ShaderResourceType::Sampler:
        return vk::DescriptorType::eSampler;
    case ShaderResourceType::AccelerationStructure:
        return vk::DescriptorType::eAccelerationStructureKHR;
    case ShaderResourceType::Unknown:
        break;
    }
    return std::unexpected(ErrorMessage("Shader binding uses an unknown resource type"));
}

[[nodiscard]] auto ToVkShaderStageFlags(ShaderStage Stages) -> vk::ShaderStageFlags {
    if (Stages == ShaderStage::Unknown)
        return vk::ShaderStageFlagBits::eAll;

    // Manual bit tests: magic_enum's bitwise operators would be ambiguous with
    // vulkan-hpp's own flag operators in this scope.
    const auto Raw = std::to_underlying(Stages);
    const auto Has = [Raw](ShaderStage Flag) { return (Raw & std::to_underlying(Flag)) != 0; };

    static constexpr std::array StageMap{
        std::pair{ShaderStage::Vertex, vk::ShaderStageFlagBits::eVertex},
        std::pair{ShaderStage::Fragment, vk::ShaderStageFlagBits::eFragment},
        std::pair{ShaderStage::Compute, vk::ShaderStageFlagBits::eCompute},
        std::pair{ShaderStage::Hull, vk::ShaderStageFlagBits::eTessellationControl},
        std::pair{ShaderStage::Domain, vk::ShaderStageFlagBits::eTessellationEvaluation},
        std::pair{ShaderStage::Geometry, vk::ShaderStageFlagBits::eGeometry},
        std::pair{ShaderStage::Mesh, vk::ShaderStageFlagBits::eMeshEXT},
        std::pair{ShaderStage::Amplification, vk::ShaderStageFlagBits::eTaskEXT},
        std::pair{ShaderStage::RayGeneration, vk::ShaderStageFlagBits::eRaygenKHR},
        std::pair{ShaderStage::Intersection, vk::ShaderStageFlagBits::eIntersectionKHR},
        std::pair{ShaderStage::AnyHit, vk::ShaderStageFlagBits::eAnyHitKHR},
        std::pair{ShaderStage::ClosestHit, vk::ShaderStageFlagBits::eClosestHitKHR},
        std::pair{ShaderStage::Miss, vk::ShaderStageFlagBits::eMissKHR},
        std::pair{ShaderStage::Callable, vk::ShaderStageFlagBits::eCallableKHR},
    };

    vk::ShaderStageFlags Result = {};
    for (const auto& [Stage, VkFlag] : StageMap)
        if (Has(Stage))
            Result |= VkFlag;
    return Result;
}

[[nodiscard]] auto GetMaxRuntimeSampledTextureCount() -> Uint32 {
    const auto& Properties = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    return std::min(
        4096U,
        std::min(Properties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                 Properties.maxDescriptorSetUpdateAfterBindSampledImages));
}

// Slang's default SPIR-V lowering assigns read-only textures to binding 2
// inside the reflected bindless descriptor space. The descriptor set index is
// not hard-coded here; it comes from ShaderReflection::BindlessSpace. The
// descriptor array's slot 0 is reserved for the zero/null material handle;
// RHIRefArray supplies descriptor-facing slots starting at 1.
inline constexpr Uint32 BindlessTextureBinding = 2;

[[nodiscard]] auto ValidateSampledImageDescriptorLimits(
    std::span<const RHIShaderBindingSet::Slot> Bindings)
    -> std::expected<void, ErrorMessage> {
    const auto& Properties = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();

    Uint64 DescriptorCount = 0;
    for (const auto& Slot : Bindings) {
        if (Slot.Info.Type != ShaderResourceType::SampledTexture)
            continue;
        DescriptorCount += Slot.Info.ArrayCount == kShaderReflectionArrayUnboundedSize
                               ? GetMaxRuntimeSampledTextureCount()
                               : Slot.Info.ArrayCount;
    }

    if (DescriptorCount > Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format(
            "Shader binding set declares {} sampled-image descriptors, exceeding the per-stage update-after-bind limit of {}",
            DescriptorCount,
            Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)));

    if (DescriptorCount > Properties.maxDescriptorSetUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format(
            "Shader binding set declares {} sampled-image descriptors, exceeding the update-after-bind descriptor-set limit of {}",
            DescriptorCount,
            Properties.maxDescriptorSetUpdateAfterBindSampledImages)));

    return {};
}

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Vulkan descriptor layouts and sets for one reflected shader binding set.
class VulkanShaderBindingSet final : public RHIShaderBindingSet {
  public:
    VulkanShaderBindingSet(const VulkanShaderBindingSet&)                    = delete;
    auto operator=(const VulkanShaderBindingSet&) -> VulkanShaderBindingSet& = delete;
    VulkanShaderBindingSet(VulkanShaderBindingSet&&)                         = delete;
    auto operator=(VulkanShaderBindingSet&&) -> VulkanShaderBindingSet&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     StringView                   Name,
                                     const RHIShaderBindingSetDesc& Desc)
        -> std::expected<UPtr<VulkanShaderBindingSet>, ErrorMessage> {
        auto Result = std::make_unique<VulkanShaderBindingSet>(
            String(Name), Desc, Context);

        const Uint32 MaxBoundDescriptorSets = VulkanCapability::Get().GetProperties().limits.maxBoundDescriptorSets;
        if (Result->m_Reflection.BindlessSpace &&
            *Result->m_Reflection.BindlessSpace >= MaxBoundDescriptorSets)
            return std::unexpected(ErrorMessage(
                Format("Shader bindless space {} exceeds the device limit of {} bound descriptor sets",
                       *Result->m_Reflection.BindlessSpace,
                       MaxBoundDescriptorSets)));

        std::optional<Uint32> MaxSet = Result->m_Reflection.BindlessSpace;
        for (const auto& Binding : Result->m_Reflection.Bindings) {
            if (Binding.Set >= MaxBoundDescriptorSets)
                return std::unexpected(ErrorMessage(
                    Format("Shader binding '{}' uses set {} which exceeds the device limit of {} bound descriptor sets",
                           Binding.ParameterPath,
                           Binding.Set,
                           MaxBoundDescriptorSets)));
            if (!MaxSet || Binding.Set > *MaxSet)
                MaxSet = Binding.Set;
        }

        std::vector<std::vector<ShaderBinding>> BindingsBySet(MaxSet ? *MaxSet + 1 : 0);
        for (const auto& Binding : Result->m_Reflection.Bindings)
            BindingsBySet[Binding.Set].push_back(Binding);

        if (auto R = ValidateSampledImageDescriptorLimits(Result->GetBindings()); !R)
            return std::unexpected(R.error());

        const auto ShaderStages = ToVkShaderStageFlags(Result->m_Stages);
        Result->m_PushConstantRanges.reserve(Result->m_Reflection.PushConstants.size());
        for (const auto& Range : Result->m_Reflection.PushConstants) {
            Result->m_PushConstantRanges.push_back(vk::PushConstantRange{
                .stageFlags = ShaderStages,
                .offset     = Range.Offset,
                .size       = Range.Size,
            });
        }
        Result->m_SetLayouts.reserve(BindingsBySet.size());
        for (Uint32 SetIndex = 0; SetIndex < BindingsBySet.size(); ++SetIndex) {
            auto& SetBindings = BindingsBySet[SetIndex];
            std::ranges::sort(SetBindings, {}, &ShaderBinding::BindingIndex);

            std::vector<vk::DescriptorSetLayoutBinding> VkBindings;
            std::vector<vk::DescriptorBindingFlags> BindingFlags;
            bool HasBindingFlags = false;
            const bool IsBindlessSet = Result->m_Reflection.BindlessSpace &&
                                       SetIndex == *Result->m_Reflection.BindlessSpace;
            VkBindings.reserve(SetBindings.size() + (IsBindlessSet ? 1 : 0));
            BindingFlags.reserve(SetBindings.size() + (IsBindlessSet ? 1 : 0));
            if (IsBindlessSet) {
                if (!SetBindings.empty())
                    return std::unexpected(
                        ErrorMessage("Bindless descriptor space overlaps ordinary shader bindings"));

                VkBindings.push_back(vk::DescriptorSetLayoutBinding{
                    .binding            = BindlessTextureBinding,
                    .descriptorType     = vk::DescriptorType::eSampledImage,
                    .descriptorCount    = GetMaxRuntimeSampledTextureCount(),
                    .stageFlags         = ShaderStages,
                    .pImmutableSamplers = nullptr,
                });
                auto BindlessFlags = vk::DescriptorBindingFlagBits::ePartiallyBound |
                                     vk::DescriptorBindingFlagBits::eUpdateAfterBind;
                if (VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>()
                        .descriptorBindingUpdateUnusedWhilePending)
                    BindlessFlags |= vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
                BindingFlags.push_back(BindlessFlags);
                HasBindingFlags = true;
            }
            for (Uint32 Index = 0; Index < SetBindings.size(); ++Index) {
                const auto& Binding = SetBindings[Index];
                auto Type = ToVkDescriptorType(Binding.Type);
                if (!Type)
                    return std::unexpected(Type.error());

                const bool IsRuntimeArray = Binding.ArrayCount == kShaderReflectionArrayUnboundedSize;
                if (IsRuntimeArray)
                    return std::unexpected(ErrorMessage(
                        "Implicit runtime arrays must use the bindless shader binding path"));
                const Uint32 Count = Binding.ArrayCount;
                if (Count == 0)
                    return std::unexpected(ErrorMessage("Shader binding descriptor count must not be zero"));

                VkBindings.push_back(vk::DescriptorSetLayoutBinding{
                    .binding            = Binding.BindingIndex,
                    .descriptorType     = *Type,
                    .descriptorCount    = Count,
                    .stageFlags         = ShaderStages,
                    .pImmutableSamplers = nullptr,
                });

                BindingFlags.push_back({});
            }

            std::optional<vk::raii::DescriptorSetLayout> Layout = std::nullopt;
            if (HasBindingFlags) {
                vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsInfo{
                    .bindingCount  = static_cast<Uint32>(BindingFlags.size()),
                    .pBindingFlags = BindingFlags.data(),
                };
                vk::StructureChain<vk::DescriptorSetLayoutCreateInfo,
                                   vk::DescriptorSetLayoutBindingFlagsCreateInfo>
                    Chain{
                        {.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                         .bindingCount = static_cast<Uint32>(VkBindings.size()),
                         .pBindings    = VkBindings.data()},
                        FlagsInfo};
                auto Created =
                    Context.GetDevice().createDescriptorSetLayout(Chain.get<vk::DescriptorSetLayoutCreateInfo>());
                if (Created.result != vk::Result::eSuccess)
                    return std::unexpected(ErrorMessage("Failed to create shader binding descriptor set layout"));
                Layout.emplace(std::move(Created.value));
            } else {
                vk::DescriptorSetLayoutCreateInfo CreateInfo{
                    .bindingCount = static_cast<Uint32>(VkBindings.size()),
                    .pBindings    = VkBindings.data(),
                };
                auto Created = Context.GetDevice().createDescriptorSetLayout(CreateInfo);
                if (Created.result != vk::Result::eSuccess)
                    return std::unexpected(ErrorMessage("Failed to create shader binding descriptor set layout"));
                Layout.emplace(std::move(Created.value));
            }

            Context.GetDebugUtils().SetObjectName(
                **Layout, Format("Internal/ShaderBindingSet/{}/Set{}", Name, Result->m_SetLayouts.size()));
            Result->m_SetLayouts.push_back(std::move(*Layout));
        }

        std::vector<vk::DescriptorSetLayout> Layouts;
        Layouts.reserve(Result->m_SetLayouts.size());
        for (const auto& Layout : Result->m_SetLayouts)
            Layouts.push_back(*Layout);
        auto Sets = Context.GetDescriptorManager().AllocateDescriptorSets(Layouts);
        if (!Sets)
            return std::unexpected(Sets.error().Append("Failed to allocate shader binding descriptor sets"));
        Result->m_Sets = std::move(*Sets);

        std::vector<vk::DescriptorSetLayout> RawSetLayouts;
        RawSetLayouts.reserve(Result->m_SetLayouts.size());
        for (const auto& Layout : Result->m_SetLayouts)
            RawSetLayouts.push_back(*Layout);

        vk::PipelineLayoutCreateInfo PipelineLayoutCI{
            .setLayoutCount         = static_cast<Uint32>(RawSetLayouts.size()),
            .pSetLayouts            = RawSetLayouts.empty() ? nullptr : RawSetLayouts.data(),
            .pushConstantRangeCount = static_cast<Uint32>(Result->m_PushConstantRanges.size()),
            .pPushConstantRanges    = Result->m_PushConstantRanges.empty()
                                          ? nullptr
                                          : Result->m_PushConstantRanges.data(),
        };
        auto PipelineLayout = Context.GetDevice().createPipelineLayout(PipelineLayoutCI);
        if (PipelineLayout.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create shader binding pipeline layout"));
        Context.GetDebugUtils().SetObjectName(*PipelineLayout.value, Format("Internal/PipelineLayout/{}", Name));
        Result->m_PipelineLayout =
            std::make_shared<vk::raii::PipelineLayout>(std::move(PipelineLayout.value));
        return Result;
    }

    [[nodiscard]] auto GetDescriptorSetLayouts() const
        -> std::span<const vk::raii::DescriptorSetLayout> {
        return m_SetLayouts;
    }

    [[nodiscard]] auto GetDescriptorSets() const
        -> std::span<const vk::raii::DescriptorSet> {
        return m_Sets;
    }

    [[nodiscard]] auto GetShaderStageFlags() const -> vk::ShaderStageFlags {
        return ToVkShaderStageFlags(m_Stages);
    }

    [[nodiscard]] auto GetPushConstantRanges() const
        -> const std::vector<vk::PushConstantRange>& {
        return m_PushConstantRanges;
    }

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout {
        return **m_PipelineLayout;
    }

    struct CommittedState {
        std::vector<Uint32>                DynamicOffsets = {};
        std::vector<PushConstantWrite> PushConstants   = {};
    };

    [[nodiscard]] auto Commit() -> std::expected<void, ErrorMessage> override {
        std::optional<vk::Buffer> UniformBuffer = std::nullopt;
        std::optional<vk::Buffer> StorageBuffer = std::nullopt;
        std::vector<Uint32> DynamicOffsets;
        for (const auto& Slot : GetBindings()) {
            if (Slot.Info.Type == ShaderResourceType::ConstantBuffer) {
                const auto* Ref = std::get_if<RHIRef<RHITransientConstantBuffer>>(&Slot.Resource);
                const auto* Buffer = Ref ? static_cast<const VulkanTransientConstantBuffer*>(Ref->TryGet()) : nullptr;
                if (!Buffer)
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' has no ready transient constant buffer", Slot.Info.ParameterPath)));
                const auto CurrentBuffer = Buffer->GetArenaBuffer();
                if (!UniformBuffer)
                    UniformBuffer = CurrentBuffer;
                else if (*UniformBuffer != CurrentBuffer)
                    return std::unexpected(ErrorMessage(
                        "All transient constant buffers in a binding set must use the same arena buffer"));
                DynamicOffsets.push_back(Buffer->GetOffset());
            } else if (Slot.Info.Type == ShaderResourceType::StorageBuffer) {
                const auto* Ref = std::get_if<RHIRef<RHITransientShaderStorageBuffer>>(&Slot.Resource);
                const auto* Buffer =
                    Ref ? static_cast<const VulkanTransientShaderStorageBuffer*>(Ref->TryGet()) : nullptr;
                if (!Buffer)
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' has no ready transient storage buffer", Slot.Info.ParameterPath)));
                const auto CurrentBuffer = Buffer->GetArenaBuffer();
                if (!StorageBuffer)
                    StorageBuffer = CurrentBuffer;
                else if (*StorageBuffer != CurrentBuffer)
                    return std::unexpected(ErrorMessage(
                        "All transient storage buffers in a binding set must use the same arena buffer"));
                if (Buffer->GetOffset() > std::numeric_limits<Uint32>::max())
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' offset exceeds the dynamic offset range", Slot.Info.ParameterPath)));
                DynamicOffsets.push_back(Buffer->GetOffset());
            }
        }
        m_CommittedStates.push_back(CommittedState{
            .DynamicOffsets = std::move(DynamicOffsets),
            .PushConstants = m_PendingPushConstants,
        });
        m_PendingPushConstants.clear();
        return {};
    }

    auto Flush() -> void {
        std::vector<vk::WriteDescriptorSet> Writes;
        for (const auto& [BindingIndex, Pending] : m_PendingWrites) {
            Writes.reserve(Writes.size() + Pending.size());
            Writes.append_range(Pending);
        }

        if (m_BindlessTextures) {
            const auto ChangedElements = m_BindlessTextures->GetChangedElements();
            for (const auto& [Element, Texture] : ChangedElements) {
                const auto* Payload = Texture.TryGet();
                if (!Payload)
                    continue;
                Writes.push_back(static_cast<const VulkanSampledTexture*>(Payload)->GetWriteDescriptorSet(
                    *m_Sets[*m_Reflection.BindlessSpace], BindlessTextureBinding, Element, true));
            }
        }
        m_Descriptors.WriteDescriptorSets(Writes);
        for (const auto& [BindingIndex, Pending] : m_PendingWrites) {
            if (Pending.empty())
                continue;
            const auto Type = GetBindings()[BindingIndex].Info.Type;
            if (Type == ShaderResourceType::ConstantBuffer || Type == ShaderResourceType::StorageBuffer)
                m_PublishedTransientBindings.insert(BindingIndex);
        }
        m_PendingWrites.clear();
    }

    [[nodiscard]] auto PopCommittedState() -> std::expected<CommittedState, ErrorMessage> {
        if (m_CommittedStates.empty())
            return std::unexpected(ErrorMessage("Shader binding set has no committed binding snapshot"));
        auto Result = std::move(m_CommittedStates.front());
        m_CommittedStates.pop_front();
        return Result;
    }

    VulkanShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc, const VulkanResourceContext& Context)
        : RHIShaderBindingSet(std::move(Name), Desc),
          m_Descriptors(Context.GetDescriptorManager()) {}

  protected:
    [[nodiscard]] auto OnResourceBound(Uint32 BindingIndex) -> std::expected<void, ErrorMessage> override {
        const auto& Binding = GetBindings()[BindingIndex];

        // Array slots are collected incrementally by Flush().
        if (Binding.Info.ArrayCount != 1)
            return {};

        const bool IsTransient = Binding.Info.Type == ShaderResourceType::ConstantBuffer ||
                                 Binding.Info.Type == ShaderResourceType::StorageBuffer;
        if (IsTransient && m_PublishedTransientBindings.contains(BindingIndex))
            return {};

        const auto Set = *m_Sets[Binding.Info.Set];

        auto QueueWrite = [&](vk::WriteDescriptorSet Write) {
            auto& Pending = m_PendingWrites[BindingIndex];
            Pending.clear();
            Pending.push_back(Write);
        };

        switch (Binding.Info.Type) {
        case ShaderResourceType::ConstantBuffer: {
            const auto* Ref = std::get_if<RHIRef<RHITransientConstantBuffer>>(&Binding.Resource);
            const auto* Buffer = Ref ? static_cast<const VulkanTransientConstantBuffer*>(Ref->TryGet()) : nullptr;
            if (!Buffer)
                return std::unexpected(ErrorMessage(
                    Format("Shader binding '{}' has no ready transient constant buffer", Binding.Info.ParameterPath)));
            QueueWrite(Buffer->GetWriteDescriptorSet(Set, Binding.Info.BindingIndex, Binding.IsReadOnly));
            break;
        }
        case ShaderResourceType::StorageBuffer: {
            const auto* Ref = std::get_if<RHIRef<RHITransientShaderStorageBuffer>>(&Binding.Resource);
            const auto* Buffer =
                Ref ? static_cast<const VulkanTransientShaderStorageBuffer*>(Ref->TryGet()) : nullptr;
            if (!Buffer)
                return std::unexpected(ErrorMessage(
                    Format("Shader binding '{}' has no ready transient storage buffer", Binding.Info.ParameterPath)));
            QueueWrite(Buffer->GetWriteDescriptorSet(Set, Binding.Info.BindingIndex, Binding.IsReadOnly));
            break;
        }
        case ShaderResourceType::SampledTexture: {
            if (const auto* Ref = std::get_if<RHIRef<RHISampledTexture>>(&Binding.Resource)) {
                const auto* Texture = Ref->TryGet();
                if (!Texture)
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' has no ready sampled texture", Binding.Info.ParameterPath)));
                QueueWrite(static_cast<const VulkanSampledTexture*>(Texture)->GetWriteDescriptorSet(
                    Set, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
            } else if (const auto* Ref = std::get_if<RHIRef<RHIRenderTarget>>(&Binding.Resource)) {
                const auto* Target = Ref->TryGet();
                if (!Target)
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' has no ready sampled render target", Binding.Info.ParameterPath)));
                QueueWrite(static_cast<const VulkanRenderTarget*>(Target)->GetWriteDescriptorSet(
                    Set, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
            }
            break;
        }
        case ShaderResourceType::StorageTexture: {
            const auto* Ref = std::get_if<RHIRef<RHIRenderTarget>>(&Binding.Resource);
            const auto* Target = Ref ? static_cast<const VulkanRenderTarget*>(Ref->TryGet()) : nullptr;
            if (!Target)
                return std::unexpected(
                    ErrorMessage(Format("Shader binding '{}' has no ready storage texture", Binding.Info.ParameterPath)));
            QueueWrite(Target->GetWriteDescriptorSet(Set, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
            break;
        }
        case ShaderResourceType::Sampler: {
            const auto* Ref = std::get_if<RHIRef<RHISampler>>(&Binding.Resource);
            const auto* Sampler = Ref ? static_cast<const VulkanSampler*>(Ref->TryGet()) : nullptr;
            if (!Sampler)
                return std::unexpected(
                    ErrorMessage(Format("Shader binding '{}' has no ready sampler", Binding.Info.ParameterPath)));
            QueueWrite(Sampler->GetWriteDescriptorSet(Set, Binding.Info.BindingIndex, Binding.IsReadOnly));
            break;
        }
        case ShaderResourceType::AccelerationStructure: {
            const auto* Ref = std::get_if<RHIRef<RHITopLevelAccelerationStructure>>(&Binding.Resource);
            const auto* Tlas =
                Ref ? static_cast<const VulkanTopLevelAccelerationStructure*>(Ref->TryGet()) : nullptr;
            if (!Tlas)
                return std::unexpected(
                    ErrorMessage(Format("Shader binding '{}' has no ready acceleration structure", Binding.Info.ParameterPath)));
            QueueWrite(Tlas->GetWriteDescriptorSet(Set, Binding.Info.BindingIndex, Binding.IsReadOnly));
            break;
        }
        case ShaderResourceType::Unknown:
            return std::unexpected(
                ErrorMessage(Format("Shader binding '{}' uses an unknown resource type", Binding.Info.ParameterPath)));
        }

        return {};
    }

  private:
    VulkanDescriptorManager&                 m_Descriptors;
    std::vector<vk::raii::DescriptorSetLayout> m_SetLayouts = {};
    std::vector<vk::raii::DescriptorSet>       m_Sets = {};
    std::vector<vk::PushConstantRange>         m_PushConstantRanges = {};
    SPtr<vk::raii::PipelineLayout>             m_PipelineLayout = nullptr;
    std::unordered_map<Uint32, std::vector<vk::WriteDescriptorSet>> m_PendingWrites = {};
    std::unordered_set<Uint32>                m_PublishedTransientBindings = {};
    std::deque<CommittedState>                m_CommittedStates = {};
};

} // namespace SoulEngine
