module RHIVulkan.ShaderBindingSet;

import RHI;
import Shader;
import vulkan;
import std;
import Core;

import RHIVulkan.AccelerationStructure;
import RHIVulkan.Buffer;
import RHIVulkan.Capability;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Descriptor;
import RHIVulkan.Sampler;
import RHIVulkan.Texture;

namespace SoulEngine {

CVar<Int32> CVarRhiVulkanMaxTextures{"RHI.Vulkan.MaxTextures", 4096, "Bindless descriptor array size cap"};

auto ToVkShaderStageFlags(ShaderStage Stages) -> vk::ShaderStageFlags {
    if (Stages == ShaderStage::Unknown)
        return vk::ShaderStageFlagBits::eAll;

    // Manual bit tests: magic_enum's bitwise operators would be ambiguous with vulkan-hpp's own flag
    // operators in this scope.
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

auto GetMaxRuntimeSampledTextureCount() -> Uint32 {
    const auto& Properties = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    return std::min(static_cast<Uint32>(CVarRhiVulkanMaxTextures.Get()),
                    std::min(Properties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                             Properties.maxDescriptorSetUpdateAfterBindSampledImages));
}

auto GetMaxDescriptorArrayCount(ShaderResourceType Type, ShaderBindingAccess Access) -> Uint32 {
    const auto& Limits   = VulkanCapability::Get().GetProperties().limits;
    Uint32      PerStage = 0;
    Uint32      PerSet   = 0;
    switch (Type) {
    case ShaderResourceType::Texture:
        // Sampled and storage images occupy separate per-stage/per-set pools.
        if (Access == ShaderBindingAccess::ReadWrite) {
            PerStage = Limits.maxPerStageDescriptorStorageImages;
            PerSet   = Limits.maxDescriptorSetStorageImages;
        } else {
            PerStage = Limits.maxPerStageDescriptorSampledImages;
            PerSet   = Limits.maxDescriptorSetSampledImages;
        }
        break;
    case ShaderResourceType::ConstantBuffer:
        PerStage = Limits.maxPerStageDescriptorUniformBuffers;
        PerSet   = Limits.maxDescriptorSetUniformBuffers;
        break;
    case ShaderResourceType::StorageBuffer:
        PerStage = Limits.maxPerStageDescriptorStorageBuffers;
        PerSet   = Limits.maxDescriptorSetStorageBuffers;
        break;
    case ShaderResourceType::Sampler:
        PerStage = Limits.maxPerStageDescriptorSamplers;
        PerSet   = Limits.maxDescriptorSetSamplers;
        break;
    default:
        return 1;
    }
    return (std::min)(static_cast<Uint32>(CVarRhiVulkanMaxTextures.Get()), (std::min)(PerStage, PerSet));
}

auto ValidateDescriptorSetIndices(const ShaderReflection& Reflection) -> std::expected<void, ErrorMessage> {
    const Uint32 MaxBoundDescriptorSets = VulkanCapability::Get().GetProperties().limits.maxBoundDescriptorSets;

    // Validate the optional bindless descriptor set index.
    if (Reflection.BindlessSpace && *Reflection.BindlessSpace >= MaxBoundDescriptorSets)
        return std::unexpected(
            ErrorMessage(Format("Shader bindless space {} exceeds the device limit of {} bound descriptor sets",
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

auto ValidateBindlessSpace(std::span<const RHIShaderBindingSet::Slot> Bindings) -> std::expected<void, ErrorMessage> {
    // The bindless space is synthesized by the backend and must not overlap with ordinary reflected bindings.
    if (!Bindings.empty())
        return std::unexpected(ErrorMessage("Bindless descriptor space overlaps ordinary shader bindings"));

    const auto&  Properties      = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    const Uint32 DescriptorCount = GetMaxRuntimeSampledTextureCount();

    if (DescriptorCount > Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format("Bindless space declares {} sampled-image descriptors, exceeding "
                                                   "the per-stage update-after-bind limit of {}",
                                                   DescriptorCount,
                                                   Properties.maxPerStageDescriptorUpdateAfterBindSampledImages)));

    if (DescriptorCount > Properties.maxDescriptorSetUpdateAfterBindSampledImages)
        return std::unexpected(ErrorMessage(Format("Bindless space declares {} sampled-image descriptors, exceeding "
                                                   "the update-after-bind descriptor-set limit of {}",
                                                   DescriptorCount,
                                                   Properties.maxDescriptorSetUpdateAfterBindSampledImages)));

    return {};
}

auto VulkanShaderBindingSet::Create(const VulkanResourceContext&   Context,
                                    VulkanDescriptorManager&       Descriptors,
                                    StringView                     Name,
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
        const bool IsBindlessSet = Result->m_BindlessSpace && SetIndex == *Result->m_BindlessSpace;
        const auto CreateLayout =
            [&Context, &Result, &Name](
                const vk::DescriptorSetLayoutCreateInfo& CreateInfo) -> std::expected<void, ErrorMessage> {
            auto Created = Context.GetDevice().createDescriptorSetLayout(CreateInfo);
            if (Created.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create shader binding descriptor set layout"));
            Context.GetDebugUtils().SetObjectName(
                *Created.value, Format("Internal/ShaderBindingSet/{}/Set{}", Name, Result->m_SetLayouts.size()));
            Result->m_OwnedSetLayouts.push_back(std::move(Created.value));
            Result->m_SetLayouts.push_back(*Result->m_OwnedSetLayouts.back());
            return {};
        };

        if (IsBindlessSet) {
            Result->m_SetLayouts.push_back(Descriptors.GetBindlessDescriptorSetLayout());
        } else {
            // Step 4b: process ordinary set layout. Fixed arrays bind with their reflected count; unbounded
            // arrays become variable descriptor count bindings. All array bindings get ePartiallyBound so
            // callers may leave array indices unbound.
            std::vector<vk::DescriptorBindingFlags> BindingFlags(SetBindings.size(), vk::DescriptorBindingFlags{});
            bool                                    HasVariableCountBinding = false;
            Uint32                                  MaxBindingIndexInSet    = 0;
            for (const auto& Binding : SetBindings)
                MaxBindingIndexInSet = (std::max)(MaxBindingIndexInSet, Binding.Info.BindingIndex);

            for (Uint32 BindingIdx = 0; BindingIdx < SetBindings.size(); ++BindingIdx) {
                const auto& Binding = SetBindings[BindingIdx];
                auto        Type    = ToVkDescriptorType(Binding.Info.Type, Binding.Info.Access);

                Uint32 DescriptorCount = Binding.Info.ArrayCount;
                if (DescriptorCount == kShaderReflectionArrayUnboundedSize) {
                    // Vulkan requires a variable descriptor count binding to be the set's last binding;
                    // shader side declares it last in the struct.
                    if (Binding.Info.BindingIndex != MaxBindingIndexInSet)
                        return std::unexpected(ErrorMessage(
                            Format("Unbounded descriptor array '{}' must be the last binding in set {}; declare it "
                                   "last in the ParameterBlock struct",
                                   Binding.Info.ParameterPath,
                                   SetIndex)));
                    DescriptorCount          = GetMaxDescriptorArrayCount(Binding.Info.Type, Binding.Info.Access);
                    BindingFlags[BindingIdx] = vk::DescriptorBindingFlagBits::ePartiallyBound |
                                               vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
                    HasVariableCountBinding  = true;
                    SetBindings[BindingIdx].IsVariableCountArray = true;
                } else {
                    const Uint32 MaxCount = GetMaxDescriptorArrayCount(Binding.Info.Type, Binding.Info.Access);
                    if (DescriptorCount > MaxCount)
                        return std::unexpected(ErrorMessage(
                            Format("Fixed descriptor array '{}' requests {} elements, exceeding the device limit of {}",
                                   Binding.Info.ParameterPath,
                                   DescriptorCount,
                                   MaxCount)));
                    if (DescriptorCount > 1)
                        BindingFlags[BindingIdx] = vk::DescriptorBindingFlagBits::ePartiallyBound;
                }
                SetBindings[BindingIdx].ArrayCapacity = DescriptorCount;

                VkBindings.push_back(vk::DescriptorSetLayoutBinding{
                    .binding            = Binding.Info.BindingIndex,
                    .descriptorType     = Type,
                    .descriptorCount    = DescriptorCount,
                    .stageFlags         = ShaderStages,
                    .pImmutableSamplers = nullptr,
                });
            }

            if (HasVariableCountBinding) {
                vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsInfo{
                    .bindingCount  = static_cast<Uint32>(BindingFlags.size()),
                    .pBindingFlags = BindingFlags.data(),
                };
                vk::StructureChain<vk::DescriptorSetLayoutCreateInfo, vk::DescriptorSetLayoutBindingFlagsCreateInfo>
                    Chain{vk::DescriptorSetLayoutCreateInfo{
                              .bindingCount = static_cast<Uint32>(VkBindings.size()),
                              .pBindings    = VkBindings.data(),
                          },
                          FlagsInfo};
                if (auto R = CreateLayout(Chain.get<vk::DescriptorSetLayoutCreateInfo>()); !R)
                    return std::unexpected(R.error());
            } else {
                vk::DescriptorSetLayoutCreateInfo CreateInfo{
                    .bindingCount = static_cast<Uint32>(VkBindings.size()),
                    .pBindings    = VkBindings.data(),
                };
                auto Created = CreateLayout(CreateInfo);
                if (!Created)
                    return std::unexpected(Created.error());
            }
        }
    }

    Result->m_SetKeys.resize(Result->m_SetLayouts.size());
    for (Uint32 SetIndex = 0; SetIndex < Result->m_SetLayouts.size(); ++SetIndex)
        Result->m_SetKeys[SetIndex].Layout = Result->m_SetLayouts[SetIndex];

    // Step 5: create the pipeline layout
    std::vector<vk::DescriptorSetLayout> RawSetLayouts;
    RawSetLayouts.reserve(Result->m_SetLayouts.size());
    for (const auto Layout : Result->m_SetLayouts)
        RawSetLayouts.push_back(Layout);

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

auto VulkanShaderBindingSet::GetDescriptorSetLayouts() const -> std::span<const vk::DescriptorSetLayout> {
    return m_SetLayouts;
}

auto VulkanShaderBindingSet::GetShaderStageFlags() const -> vk::ShaderStageFlags {
    return ToVkShaderStageFlags(m_Stages);
}

auto VulkanShaderBindingSet::GetPushConstantRanges() const -> const std::vector<vk::PushConstantRange>& {
    return m_PushConstantRanges;
}

auto VulkanShaderBindingSet::GetPipelineLayout() const -> vk::PipelineLayout {
    return *m_PipelineLayout;
}

auto VulkanShaderBindingSet::CaptureBindingState() -> std::expected<PipelineBindingState, ErrorMessage> {
    if (HasUnboundBindings())
        return std::unexpected(ErrorMessage(Format("Shader binding set '{}' has unbound resources", GetName())));

    PipelineBindingState State;
    State.DescriptorSets.reserve(m_SetKeys.size());
    // Lazily load descriptor sets from cache, and update LRU
    for (Uint32 SetIndex = 0; SetIndex < m_SetKeys.size(); ++SetIndex) {
        const auto Set = m_BindlessSpace && SetIndex == *m_BindlessSpace
                             ? m_Descriptors.GetBindlessDescriptorSet()
                             : m_Descriptors.AcquireDescriptorSet(m_SetKeys[SetIndex], m_BindingsBySet[SetIndex]);
        if (!Set)
            return std::unexpected(ErrorMessage(
                Format("Failed to acquire descriptor set {} for shader binding set '{}'", SetIndex, GetName())));
        State.DescriptorSets.push_back(Set);
    }

    std::optional<vk::Buffer> UniformBuffer = std::nullopt;
    std::optional<vk::Buffer> StorageBuffer = std::nullopt;
    std::vector<Uint32>       DynamicOffsets;
    for (const auto& SetBindings : m_BindingsBySet)
        for (const auto& Slot : SetBindings) {
            if (Slot.Info.Type == ShaderResourceType::ConstantBuffer ||
                Slot.Info.Type == ShaderResourceType::StorageBuffer) {
                const auto* View   = std::get_if<RHIBufferView>(&Slot.Bound);
                const auto* Buffer = View ? static_cast<const VulkanBuffer*>(View->Buffer.TryGet()) : nullptr;
                const auto* Prop   = Buffer ? Buffer->GetProp<TransientBufferProp>() : nullptr;
                if (!Prop)
                    return std::unexpected(ErrorMessage(
                        Format("Shader binding '{}' has no ready transient buffer", Slot.Info.ParameterPath)));
                auto& BeltSlot = Slot.Info.Type == ShaderResourceType::ConstantBuffer ? UniformBuffer : StorageBuffer;
                const auto CurrentBuffer = Buffer->GetVkBuffer();
                if (!BeltSlot)
                    BeltSlot = CurrentBuffer;
                else if (*BeltSlot != CurrentBuffer)
                    return std::unexpected(ErrorMessage(
                        "All transient buffers of one kind in a binding set must use the same belt buffer"));
                DynamicOffsets.push_back(static_cast<Uint32>(Prop->Allocation.Offset));
            }
        }
    State.DynamicOffsets = std::move(DynamicOffsets);
    State.PushConstants  = std::move(m_PendingPushConstants);
    m_PendingPushConstants.clear();
    return State;
}

VulkanShaderBindingSet::VulkanShaderBindingSet(String                         Name,
                                               const RHIShaderBindingSetDesc& Desc,
                                               VulkanDescriptorManager&       Descriptors)
    : RHIShaderBindingSet(std::move(Name), Desc), m_Descriptors(Descriptors) {}

auto VulkanShaderBindingSet::OnResourcesBound(Uint32 SetIndex) -> std::expected<void, ErrorMessage> {
    // we update descriptor set key, so that `CaptureBindingState()` later fetches the corresponding
    // descriptor set from the manager
    const auto& SetBindings = m_BindingsBySet[SetIndex];
    if (m_BindlessSpace && SetIndex == *m_BindlessSpace)
        return {};
    DescriptorSetCacheKey Key{.Layout = m_SetLayouts[SetIndex]};
    for (const auto& Slot : SetBindings) {
        // The cache key captures the full view identity: resource id plus role and range, so a re-view of the
        // same resource refreshes.
        if (const auto* View = std::get_if<RHITextureView>(&Slot.Bound)) {
            Key.ResourceIds.push_back(View->Texture ? View->Texture->GetId() : 0);
            Key.ResourceIds.push_back(static_cast<Uint32>(View->Usage));
            Key.ResourceIds.push_back(View->Range.BaseMipLevel);
            Key.ResourceIds.push_back(View->Range.MipLevelCount);
        } else if (const auto* View = std::get_if<RHIBufferView>(&Slot.Bound)) {
            Key.ResourceIds.push_back(View->Buffer ? View->Buffer->GetId() : 0);
            Key.ResourceIds.push_back(static_cast<Uint32>(View->Usage));
        } else if (const auto* View = std::get_if<RHISamplerView>(&Slot.Bound)) {
            Key.ResourceIds.push_back(View->Sampler ? View->Sampler->GetId() : 0);
        } else if (const auto* View = std::get_if<RHITlasView>(&Slot.Bound)) {
            Key.ResourceIds.push_back(View->Tlas ? View->Tlas->GetId() : 0);
        } else {
            Key.ResourceIds.push_back(0);
        }
        // Array slots: one (index, resource id, role, range) tuple per bound element. The sequence also pins
        // the variable count.
        for (const auto& Element : Slot.ArrayElements) {
            Key.ResourceIds.push_back(Element.ArrayIndex);
            Key.ResourceIds.push_back(Element.View.Texture ? Element.View.Texture->GetId() : 0);
            Key.ResourceIds.push_back(static_cast<Uint32>(Element.View.Usage));
            Key.ResourceIds.push_back(Element.View.Range.BaseMipLevel);
            Key.ResourceIds.push_back(Element.View.Range.MipLevelCount);
        }
    }
    m_SetKeys[SetIndex] = std::move(Key);
    return {};
}

} // namespace SoulEngine
