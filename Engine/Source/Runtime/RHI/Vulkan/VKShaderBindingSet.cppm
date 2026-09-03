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

[[nodiscard]] auto ToVkDescriptorType(ShaderResourceType Type) -> vk::DescriptorType {
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
    default:
        return {};
    }
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

[[nodiscard]] auto ValidateDescriptorSetIndices(const ShaderReflection& Reflection)
    -> std::expected<void, ErrorMessage> {
    const Uint32 MaxBoundDescriptorSets =
        VulkanCapability::Get().GetProperties().limits.maxBoundDescriptorSets;

    // Validate the optional bindless descriptor set index.
    if (Reflection.BindlessSpace && *Reflection.BindlessSpace >= MaxBoundDescriptorSets)
        return std::unexpected(ErrorMessage(
            Format("Shader bindless space {} exceeds the device limit of {} bound descriptor sets",
                   *Reflection.BindlessSpace,
                   MaxBoundDescriptorSets)));

    // Validate every reflected binding against the device descriptor-set limit.
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Set >= MaxBoundDescriptorSets)
            return std::unexpected(ErrorMessage(
                Format("Shader binding '{}' uses set {} which exceeds the device limit of {} bound descriptor sets",
                       Binding.ParameterPath,
                       Binding.Set,
                       MaxBoundDescriptorSets)));
    }
    return {};
}

[[nodiscard]] auto ValidateBindlessSpace(std::span<const RHIShaderBindingSet::Slot> Bindings)
    -> std::expected<void, ErrorMessage> {
    // The bindless space is synthesized by the backend and must not overlap
    // with ordinary reflected bindings.
    if (!Bindings.empty())
        return std::unexpected(ErrorMessage("Bindless descriptor space overlaps ordinary shader bindings"));

    const auto& Properties = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    const Uint32 DescriptorCount = GetMaxRuntimeSampledTextureCount();

    if (DescriptorCount > Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format(
            "Bindless space declares {} sampled-image descriptors, exceeding the per-stage update-after-bind limit of {}",
            DescriptorCount,
            Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)));

    if (DescriptorCount > Properties.maxDescriptorSetUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format(
            "Bindless space declares {} sampled-image descriptors, exceeding the update-after-bind descriptor-set limit of {}",
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
                                     VulkanDescriptorManager&      Descriptors,
                                     StringView                   Name,
                                     const RHIShaderBindingSetDesc& Desc)
        -> std::expected<UPtr<VulkanShaderBindingSet>, ErrorMessage> {
        auto Result = std::make_unique<VulkanShaderBindingSet>(String(Name), Desc, Descriptors);

        // Step 1: validate reflected descriptor-set indices against device limits.
        if (auto R = ValidateDescriptorSetIndices(Desc.Reflection); !R)
            return std::unexpected(R.error());

        // Step 2: validate the bindless space
        if (Result->m_BindlessSpace) {
            if (auto R = ValidateBindlessSpace(Result->m_BindingsBySet[*Result->m_BindlessSpace]); !R)
                return std::unexpected(R.error());
        }

        // Step 3: build push-constant ranges
        const auto ShaderStages = ToVkShaderStageFlags(Result->m_Stages);
        Result->m_PushConstantRanges.reserve(Desc.Reflection.PushConstants.size());
        for (const auto& Range : Desc.Reflection.PushConstants) {
            Result->m_PushConstantRanges.push_back(vk::PushConstantRange{
                .stageFlags = ShaderStages,
                .offset     = Range.Offset,
                .size       = Range.Size,
            });
        }

        // Step 4: Allocate all set layouts
        Result->m_SetLayouts.reserve(Result->m_BindingsBySet.size());
        for (Uint32 SetIndex = 0; SetIndex < Result->m_BindingsBySet.size(); ++SetIndex) {
            auto& SetBindings = Result->m_BindingsBySet[SetIndex];

            std::vector<vk::DescriptorSetLayoutBinding> VkBindings = {};
            const bool IsBindlessSet = Result->m_BindlessSpace &&
                                       SetIndex == *Result->m_BindlessSpace;
            const auto CreateLayout = [&Context, &Result, &Name](const vk::DescriptorSetLayoutCreateInfo& CreateInfo)
                -> std::expected<void, ErrorMessage> {
                auto Created = Context.GetDevice().createDescriptorSetLayout(CreateInfo);
                if (Created.result != vk::Result::eSuccess)
                    return std::unexpected(ErrorMessage("Failed to create shader binding descriptor set layout"));
                Context.GetDebugUtils().SetObjectName(
                    *Created.value,
                    Format("Internal/ShaderBindingSet/{}/Set{}", Name, Result->m_SetLayouts.size()));
                Result->m_SetLayouts.push_back(std::move(Created.value));
                return {};
            };

            if (IsBindlessSet) {
                // Step 4a: process bindless space set layout
                VkBindings.push_back(vk::DescriptorSetLayoutBinding{
                    .binding            = BindlessTextureBinding,
                    // we only support bindless textures for now,
                    // which is hard coded here
                    // TODO: support more types
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
                vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsInfo{
                    .bindingCount = 1,
                    .pBindingFlags = &BindlessFlags,
                };
                vk::StructureChain<vk::DescriptorSetLayoutCreateInfo,
                                   vk::DescriptorSetLayoutBindingFlagsCreateInfo>
                    Chain{
                        {.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                         .bindingCount = static_cast<Uint32>(VkBindings.size()),
                         .pBindings    = VkBindings.data()},
                        FlagsInfo};
                auto Created = CreateLayout(Chain.get<vk::DescriptorSetLayoutCreateInfo>());
                if (!Created)
                    return std::unexpected(Created.error());
            } else {
                // Step 4b: process ordinary set layout
                for (const auto& Binding : SetBindings) {
                    auto Type = ToVkDescriptorType(Binding.Info.Type);
                    VkBindings.push_back(vk::DescriptorSetLayoutBinding{
                        .binding            = Binding.Info.BindingIndex,
                        .descriptorType     = Type,
                        .descriptorCount    = 1,
                        .stageFlags         = ShaderStages,
                        .pImmutableSamplers = nullptr,
                    });
                }

                vk::DescriptorSetLayoutCreateInfo CreateInfo{
                    .bindingCount = static_cast<Uint32>(VkBindings.size()),
                    .pBindings    = VkBindings.data(),
                };
                auto Created = CreateLayout(CreateInfo);
                if (!Created)
                    return std::unexpected(Created.error());
            }

        }

        Result->m_SetKeys.resize(Result->m_SetLayouts.size());
        for (Uint32 SetIndex = 0; SetIndex < Result->m_SetLayouts.size(); ++SetIndex)
            Result->m_SetKeys[SetIndex].Layout = *Result->m_SetLayouts[SetIndex];

        // Step 5: create the pipeline layout
        std::vector<vk::DescriptorSetLayout> RawSetLayouts;
        RawSetLayouts.reserve(Result->m_SetLayouts.size());
        for (const auto& Layout : Result->m_SetLayouts)
            RawSetLayouts.push_back(*Layout);

        vk::PipelineLayoutCreateInfo PipelineLayoutCI{
            .setLayoutCount         = static_cast<Uint32>(RawSetLayouts.size()),
            .pSetLayouts            = RawSetLayouts.data(),
            .pushConstantRangeCount = static_cast<Uint32>(Result->m_PushConstantRanges.size()),
            .pPushConstantRanges    = Result->m_PushConstantRanges.data(),
        };
        auto PipelineLayout = Context.GetDevice().createPipelineLayout(PipelineLayoutCI);
        if (PipelineLayout.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create shader binding pipeline layout"));
        Context.GetDebugUtils().SetObjectName(*PipelineLayout.value, Format("Internal/PipelineLayout/{}", Name));
        Result->m_PipelineLayout = std::move(PipelineLayout.value);

        return Result;
    }

    [[nodiscard]] auto GetDescriptorSetLayouts() const
        -> std::span<const vk::raii::DescriptorSetLayout> {
        return m_SetLayouts;
    }

    [[nodiscard]] auto GetShaderStageFlags() const -> vk::ShaderStageFlags {
        return ToVkShaderStageFlags(m_Stages);
    }

    [[nodiscard]] auto GetPushConstantRanges() const
        -> const std::vector<vk::PushConstantRange>& {
        return m_PushConstantRanges;
    }

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout {
        return *m_PipelineLayout;
    }

    /// Resolves the current resource combination for every set through the
    /// device-wide descriptor cache. Cache hits also refresh the entry LRU.
    struct PipelineBindingState {
        std::vector<vk::DescriptorSet> DescriptorSets = {};
        std::vector<Uint32> DynamicOffsets = {};
        std::vector<PushConstantWrite> PushConstants = {};
    };

    [[nodiscard]] auto CaptureBindingState() -> std::expected<PipelineBindingState, ErrorMessage> {
        if (HasUnboundBindings())
            return std::unexpected(ErrorMessage(
                Format("Shader binding set '{}' has unbound resources", GetName())));

        PipelineBindingState State;
        State.DescriptorSets.reserve(m_SetKeys.size());
        // Lazily load descriptor sets from cache, and update LRU
        for (Uint32 SetIndex = 0; SetIndex < m_SetKeys.size(); ++SetIndex) {
            const auto Set = m_Descriptors.AcquireDescriptorSet(
                m_SetKeys[SetIndex], m_BindingsBySet[SetIndex]);
            if (!Set)
                return std::unexpected(ErrorMessage(
                    Format("Failed to acquire descriptor set {} for shader binding set '{}'", SetIndex, GetName())));
            State.DescriptorSets.push_back(Set);
        }

        std::optional<vk::Buffer> UniformBuffer = std::nullopt;
        std::optional<vk::Buffer> StorageBuffer = std::nullopt;
        std::vector<Uint32> DynamicOffsets;
        for (const auto& SetBindings : m_BindingsBySet)
            for (const auto& Slot : SetBindings) {
            if (Slot.Info.Type == ShaderResourceType::ConstantBuffer) {
                const auto* Buffer = static_cast<const VulkanTransientConstantBuffer*>(Slot.Resource);
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
                const auto* Buffer = static_cast<const VulkanTransientShaderStorageBuffer*>(Slot.Resource);
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
        State.DynamicOffsets = std::move(DynamicOffsets);
        State.PushConstants = std::move(m_PendingPushConstants);
        m_PendingPushConstants.clear();
        return State;
    }

    VulkanShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc, VulkanDescriptorManager& Descriptors)
        : RHIShaderBindingSet(std::move(Name), Desc),
          m_Descriptors(Descriptors) {}

  protected:
    [[nodiscard]] auto OnBindlessResourceBound() -> std::expected<void, ErrorMessage> override {
        if (!m_BindlessSpace || !m_BindlessTextures)
            return std::unexpected(ErrorMessage("Bindless resource is not configured"));

        const auto SetIndex = *m_BindlessSpace;
        const auto Set = m_Descriptors.AcquireDescriptorSet(m_SetKeys[SetIndex], m_BindingsBySet[SetIndex]);
        if (!Set)
            return std::unexpected(ErrorMessage("Failed to acquire bindless descriptor set"));

        std::vector<vk::WriteDescriptorSet> Writes;
        for (const auto& [Element, TextureRef] : m_BindlessTextures->GetChangedElements()) {
            const auto* Texture = TextureRef.TryGet();
            if (!Texture)
                continue;
            Writes.push_back(static_cast<const VulkanSampledTexture*>(Texture)->GetWriteDescriptorSet(
                Set, BindlessTextureBinding, Element, true));
        }
        m_Descriptors.WriteDescriptorSets(Writes);
        return {};
    }

    /// Updates the current set snapshot for the pass being recorded. RHI
    /// validation has already resolved and updated every slot in this set.
    /// Execute()
    /// calls Pass::Record() immediately before constructing its command visitor,
    /// so replacing this snapshot is intentional: CaptureBindingState() observes
    /// the bindings of the current pass and refreshes their cache LRU entries.
    [[nodiscard]] auto OnResourcesBound(Uint32 SetIndex)
        -> std::expected<void, ErrorMessage> override {
        // we update descriptor set key, so that `CaptureBindingState()` later
        // fetches the corresponding descriptor set from the manager
        const auto& SetBindings = m_BindingsBySet[SetIndex];
        DescriptorSetCacheKey Key{.Layout = *m_SetLayouts[SetIndex]};
        for (const auto& Slot : SetBindings) {
            Key.ResourceIds.push_back(Slot.Resource ? Slot.Resource->GetId() : 0);
            Key.ResourceIds.push_back(Slot.IsReadOnly ? 1 : 0);
        }
        m_SetKeys[SetIndex] = std::move(Key);
        return {};
    }

  private:
    VulkanDescriptorManager&                 m_Descriptors;
    std::vector<vk::raii::DescriptorSetLayout> m_SetLayouts = {};
    std::vector<DescriptorSetCacheKey>             m_SetKeys = {};
    std::vector<vk::PushConstantRange>         m_PushConstantRanges = {};
    vk::raii::PipelineLayout                   m_PipelineLayout = nullptr;
};

} // namespace SoulEngine
