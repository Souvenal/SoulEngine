module;

#include <vk_mem_alloc.h>

export module Vulkan:RayTracingPipeline;

import RHI;
import Shader;
import vulkan;
import std;

import :Buffer;
import :Capability;
import :Pipeline;
import :Context;

export namespace SoulEngine {

/// Backend-independent layout for the immutable shader-binding-table payload.
struct VulkanShaderBindingTableRegionLayout {
    Uint64 Offset      = 0;
    Uint64 Stride      = 0;
    Uint64 Size        = 0;
    Uint32 RecordCount = 0;
};

/// SBT allocation layout derived from Vulkan ray-tracing pipeline properties.
struct VulkanShaderBindingTableLayout {
    Uint64                           HandleSize = 0;
    Uint64                           RecordStride = 0;
    Uint64                           TotalSize = 0;
    VulkanShaderBindingTableRegionLayout   RayGeneration = {};
    VulkanShaderBindingTableRegionLayout   Miss = {};
    VulkanShaderBindingTableRegionLayout   Hit = {};
    VulkanShaderBindingTableRegionLayout   Callable = {};
};

/// Calculate SBT offsets while preserving Vulkan's handle and base-alignment invariants.
[[nodiscard]] auto VulkanCreateShaderBindingTableLayout(Uint32 HandleSize,
                                                   Uint32 HandleAlignment,
                                                   Uint32 BaseAlignment,
                                                   Uint32 MaxShaderGroupStride,
                                                   Uint32 RayGenerationRecordCount,
                                                   Uint32 MissRecordCount,
                                                   Uint32 HitRecordCount,
                                                   Uint32 CallableRecordCount)
    -> std::expected<VulkanShaderBindingTableLayout, ErrorMessage>;

} // namespace SoulEngine

namespace SoulEngine {

namespace {

[[nodiscard]] auto AlignUp(Uint64 Value, Uint64 Alignment) -> std::expected<Uint64, ErrorMessage> {
    if (Alignment == 0)
        return std::unexpected(ErrorMessage("SBT alignment must be non-zero"));
    const Uint64 Remainder = Value % Alignment;
    if (Remainder == 0)
        return Value;
    const Uint64 Padding = Alignment - Remainder;
    if (Value > std::numeric_limits<Uint64>::max() - Padding)
        return std::unexpected(ErrorMessage("SBT size alignment overflow"));
    return Value + Padding;
}

[[nodiscard]] auto AddRecordRegion(Uint64&                         Cursor,
                                   Uint64                          RecordStride,
                                   Uint64                          BaseAlignment,
                                   Uint32                          RecordCount,
                                   VulkanShaderBindingTableRegionLayout& Out)
    -> std::expected<void, ErrorMessage> {
    Out = {.Offset = 0, .Stride = 0, .Size = 0, .RecordCount = RecordCount};
    if (RecordCount == 0)
        return {};

    auto AlignedOffset = AlignUp(Cursor, BaseAlignment);
    if (!AlignedOffset)
        return std::unexpected(AlignedOffset.error());
    if (RecordCount > std::numeric_limits<Uint64>::max() / RecordStride)
        return std::unexpected(ErrorMessage("SBT region size overflow"));

    Out.Offset = *AlignedOffset;
    Out.Stride = RecordStride;
    Out.Size   = static_cast<Uint64>(RecordCount) * RecordStride;
    if (Out.Offset > std::numeric_limits<Uint64>::max() - Out.Size)
        return std::unexpected(ErrorMessage("SBT total size overflow"));
    Cursor = Out.Offset + Out.Size;
    return {};
}

[[nodiscard]] auto ToVkRayTracingShaderStage(ShaderStage Stage)
    -> std::expected<vk::ShaderStageFlagBits, ErrorMessage> {
    switch (Stage) {
    case ShaderStage::RayGeneration:
        return vk::ShaderStageFlagBits::eRaygenKHR;
    case ShaderStage::Intersection:
        return vk::ShaderStageFlagBits::eIntersectionKHR;
    case ShaderStage::AnyHit:
        return vk::ShaderStageFlagBits::eAnyHitKHR;
    case ShaderStage::ClosestHit:
        return vk::ShaderStageFlagBits::eClosestHitKHR;
    case ShaderStage::Miss:
        return vk::ShaderStageFlagBits::eMissKHR;
    case ShaderStage::Callable:
        return vk::ShaderStageFlagBits::eCallableKHR;
    case ShaderStage::Unknown:
    case ShaderStage::Vertex:
    case ShaderStage::Hull:
    case ShaderStage::Domain:
    case ShaderStage::Geometry:
    case ShaderStage::Fragment:
    case ShaderStage::Compute:
    case ShaderStage::Mesh:
    case ShaderStage::Amplification:
        break;
    }
    return std::unexpected(ErrorMessage("Shader stage is not valid for a Vulkan ray-tracing pipeline"));
}

[[nodiscard]] auto GetRayTracingShaderStageFlags() -> vk::ShaderStageFlags {
    return vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eIntersectionKHR |
           vk::ShaderStageFlagBits::eAnyHitKHR | vk::ShaderStageFlagBits::eClosestHitKHR |
           vk::ShaderStageFlagBits::eMissKHR | vk::ShaderStageFlagBits::eCallableKHR;
}

struct VulkanRayTracingShaderGroupIndices {
    Uint32              RayGeneration = vk::ShaderUnusedKHR;
    std::vector<Uint32> Miss = {};
    std::vector<Uint32> Hit = {};
    std::vector<Uint32> Callable = {};
};

struct VulkanRayTracingShaderStates {
    vk::raii::ShaderModule                                  Module = nullptr;
    std::vector<vk::PipelineShaderStageCreateInfo>          Stages = {};
    std::vector<vk::RayTracingShaderGroupCreateInfoKHR>     Groups = {};
    VulkanRayTracingShaderGroupIndices                             GroupIndices = {};
};

[[nodiscard]] auto CreateRayTracingShaderStates(vk::raii::Device& Device, const RHIRayTracingPipelineDesc& Desc)
    -> std::expected<VulkanRayTracingShaderStates, ErrorMessage> {
    const auto& Program = Desc.Program;
    if (Program.Code.empty())
        return std::unexpected(ErrorMessage("Ray-tracing shader program has no SPIR-V code"));
    if (Program.RayGenerationEntryPointName.empty())
        return std::unexpected(ErrorMessage("Ray-tracing shader program has no ray-generation entry point"));

    vk::ShaderModuleCreateInfo ModuleCI{
        .codeSize = Program.Code.size() * sizeof(Uint32),
        .pCode    = Program.Code.data(),
    };
    auto [ModuleResult, Module] = Device.createShaderModule(ModuleCI);
    if (ModuleResult != vk::Result::eSuccess) {
        return std::unexpected(ErrorMessage(
            Format("Failed to create shader module for ray-tracing program '{}': {}",
                         Program.RayGenerationEntryPointName,
                         vk::to_string(ModuleResult))));
    }

    VulkanRayTracingShaderStates Result{.Module = std::move(Module)};
    const auto AppendStage = [&](vk::ShaderStageFlagBits Stage, const String& EntryPoint) -> Uint32 {
        Result.Stages.push_back(vk::PipelineShaderStageCreateInfo{
            .stage  = Stage,
            .module = *Result.Module,
            .pName  = EntryPoint.c_str(),
        });
        return static_cast<Uint32>(Result.Stages.size() - 1);
    };
    const auto AppendOptionalStage = [&](vk::ShaderStageFlagBits Stage, const std::optional<String>& EntryPoint)
        -> Uint32 {
        if (!EntryPoint.has_value())
            return vk::ShaderUnusedKHR;
        return AppendStage(Stage, *EntryPoint);
    };

    const Uint32 RayGenerationStage = AppendStage(vk::ShaderStageFlagBits::eRaygenKHR, Program.RayGenerationEntryPointName);
    Result.Groups.push_back(vk::RayTracingShaderGroupCreateInfoKHR{
        .type              = vk::RayTracingShaderGroupTypeKHR::eGeneral,
        .generalShader     = RayGenerationStage,
        .closestHitShader  = vk::ShaderUnusedKHR,
        .anyHitShader      = vk::ShaderUnusedKHR,
        .intersectionShader = vk::ShaderUnusedKHR,
    });
    Result.GroupIndices.RayGeneration = 0;

    for (const auto& EntryPoint : Program.MissEntryPointNames) {
        const Uint32 Stage = AppendStage(vk::ShaderStageFlagBits::eMissKHR, EntryPoint);
        Result.Groups.push_back(vk::RayTracingShaderGroupCreateInfoKHR{
            .type               = vk::RayTracingShaderGroupTypeKHR::eGeneral,
            .generalShader      = Stage,
            .closestHitShader   = vk::ShaderUnusedKHR,
            .anyHitShader       = vk::ShaderUnusedKHR,
            .intersectionShader = vk::ShaderUnusedKHR,
        });
        Result.GroupIndices.Miss.push_back(static_cast<Uint32>(Result.Groups.size() - 1));
    }

    for (const auto& HitGroup : Program.HitGroups) {
        const Uint32 ClosestHit = AppendOptionalStage(vk::ShaderStageFlagBits::eClosestHitKHR, HitGroup.ClosestHitEntryPointName);
        const Uint32 AnyHit = AppendOptionalStage(vk::ShaderStageFlagBits::eAnyHitKHR, HitGroup.AnyHitEntryPointName);
        const Uint32 Intersection =
            AppendOptionalStage(vk::ShaderStageFlagBits::eIntersectionKHR, HitGroup.IntersectionEntryPointName);

        vk::RayTracingShaderGroupTypeKHR Type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
        switch (HitGroup.Type) {
        case ShaderRayTracingHitGroupType::Triangles:
            if (Intersection != vk::ShaderUnusedKHR)
                return std::unexpected(ErrorMessage("Triangle hit groups cannot contain an intersection shader"));
            break;
        case ShaderRayTracingHitGroupType::Procedural:
            if (Intersection == vk::ShaderUnusedKHR)
                return std::unexpected(ErrorMessage("Procedural hit groups require an intersection shader"));
            Type = vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup;
            break;
        case ShaderRayTracingHitGroupType::Unknown:
            return std::unexpected(ErrorMessage("Ray-tracing hit group has an unknown type"));
        }

        Result.Groups.push_back(vk::RayTracingShaderGroupCreateInfoKHR{
            .type               = Type,
            .generalShader      = vk::ShaderUnusedKHR,
            .closestHitShader   = ClosestHit,
            .anyHitShader       = AnyHit,
            .intersectionShader = Intersection,
        });
        Result.GroupIndices.Hit.push_back(static_cast<Uint32>(Result.Groups.size() - 1));
    }

    for (const auto& EntryPoint : Program.CallableEntryPointNames) {
        const Uint32 Stage = AppendStage(vk::ShaderStageFlagBits::eCallableKHR, EntryPoint);
        Result.Groups.push_back(vk::RayTracingShaderGroupCreateInfoKHR{
            .type               = vk::RayTracingShaderGroupTypeKHR::eGeneral,
            .generalShader      = Stage,
            .closestHitShader   = vk::ShaderUnusedKHR,
            .anyHitShader       = vk::ShaderUnusedKHR,
            .intersectionShader = vk::ShaderUnusedKHR,
        });
        Result.GroupIndices.Callable.push_back(static_cast<Uint32>(Result.Groups.size() - 1));
    }

    if (!Desc.ShaderGroups.empty()) {
        if (Desc.ShaderGroups.size() != Result.Groups.size()) {
            return std::unexpected(ErrorMessage(Format(
                "Ray-tracing pipeline descriptor specifies {} shader groups, but the linked program produces {} groups",
                Desc.ShaderGroups.size(),
                Result.Groups.size())));
        }
        for (Uint32 GroupIndex = 0; GroupIndex < Desc.ShaderGroups.size(); ++GroupIndex) {
            const auto Expected = Desc.ShaderGroups[GroupIndex].Type;
            const auto Actual = Result.Groups[GroupIndex].type;
            const bool bMatches =
                (Expected == RHIRayTracingShaderGroupType::General && Actual == vk::RayTracingShaderGroupTypeKHR::eGeneral) ||
                (Expected == RHIRayTracingShaderGroupType::TrianglesHit &&
                 Actual == vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup) ||
                (Expected == RHIRayTracingShaderGroupType::ProceduralHit &&
                 Actual == vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup);
            if (!bMatches) {
                return std::unexpected(ErrorMessage(Format(
                    "Ray-tracing shader group {} does not match the linked program's group category", GroupIndex)));
            }
        }
    }

    return Result;
}

[[nodiscard]] auto MakeRegion(vk::DeviceAddress Address, const VulkanShaderBindingTableRegionLayout& Layout)
    -> vk::StridedDeviceAddressRegionKHR {
    if (Layout.RecordCount == 0)
        return {};
    return vk::StridedDeviceAddressRegionKHR{
        .deviceAddress = Address + Layout.Offset,
        .stride        = Layout.Stride,
        .size          = Layout.Size,
    };
}

[[nodiscard]] auto PopulateShaderBindingTableData(const std::vector<Uint8>&                       Handles,
                                                   Uint32                                            HandleSize,
                                                   const VulkanRayTracingShaderGroupIndices&               Groups,
                                                   const VulkanShaderBindingTableLayout&                   Layout)
    -> std::expected<std::vector<Uint8>, ErrorMessage> {
    std::vector<Uint8> Data(Layout.TotalSize, 0);
    const auto WriteRegion = [&](const std::vector<Uint32>& GroupIndices, const VulkanShaderBindingTableRegionLayout& Region)
        -> std::expected<void, ErrorMessage> {
        if (GroupIndices.size() != Region.RecordCount)
            return std::unexpected(ErrorMessage("SBT group-index count does not match region record count"));
        for (Uint32 RecordIndex = 0; RecordIndex < GroupIndices.size(); ++RecordIndex) {
            const Uint32 GroupIndex = GroupIndices[RecordIndex];
            const Uint64 HandleOffset = static_cast<Uint64>(GroupIndex) * HandleSize;
            const Uint64 Destination = Region.Offset + static_cast<Uint64>(RecordIndex) * Region.Stride;
            if (HandleOffset + HandleSize > Handles.size() || Destination + HandleSize > Data.size())
                return std::unexpected(ErrorMessage("SBT shader group handle copy exceeds allocated storage"));
            std::memcpy(Data.data() + Destination, Handles.data() + HandleOffset, HandleSize);
        }
        return {};
    };

    if (Groups.RayGeneration == vk::ShaderUnusedKHR)
        return std::unexpected(ErrorMessage("SBT is missing its ray-generation shader group"));
    if (auto R = WriteRegion(std::vector<Uint32>{Groups.RayGeneration}, Layout.RayGeneration); !R)
        return std::unexpected(R.error());
    if (auto R = WriteRegion(Groups.Miss, Layout.Miss); !R)
        return std::unexpected(R.error());
    if (auto R = WriteRegion(Groups.Hit, Layout.Hit); !R)
        return std::unexpected(R.error());
    if (auto R = WriteRegion(Groups.Callable, Layout.Callable); !R)
        return std::unexpected(R.error());
    return Data;
}

} // namespace

export auto VulkanCreateShaderBindingTableLayout(Uint32 HandleSize,
                                           Uint32 HandleAlignment,
                                           Uint32 BaseAlignment,
                                           Uint32 MaxShaderGroupStride,
                                           Uint32 RayGenerationRecordCount,
                                           Uint32 MissRecordCount,
                                           Uint32 HitRecordCount,
                                           Uint32 CallableRecordCount)
    -> std::expected<VulkanShaderBindingTableLayout, ErrorMessage> {
    if (HandleSize == 0 || HandleAlignment == 0 || BaseAlignment == 0)
        return std::unexpected(ErrorMessage("Vulkan ray-tracing SBT properties must be non-zero"));
    if (RayGenerationRecordCount != 1)
        return std::unexpected(ErrorMessage("A Vulkan ray-tracing pipeline requires exactly one ray-generation SBT record"));

    auto RecordStride = AlignUp(HandleSize, HandleAlignment);
    if (!RecordStride)
        return std::unexpected(RecordStride.error());
    if (*RecordStride > MaxShaderGroupStride) {
        return std::unexpected(ErrorMessage(Format(
            "SBT record stride {} exceeds device maxShaderGroupStride {}", *RecordStride, MaxShaderGroupStride)));
    }

    VulkanShaderBindingTableLayout Result{
        .HandleSize   = HandleSize,
        .RecordStride = *RecordStride,
    };
    Uint64 Cursor = 0;
    if (auto R = AddRecordRegion(Cursor, Result.RecordStride, BaseAlignment, RayGenerationRecordCount, Result.RayGeneration); !R)
        return std::unexpected(R.error());
    if (auto R = AddRecordRegion(Cursor, Result.RecordStride, BaseAlignment, MissRecordCount, Result.Miss); !R)
        return std::unexpected(R.error());
    if (auto R = AddRecordRegion(Cursor, Result.RecordStride, BaseAlignment, HitRecordCount, Result.Hit); !R)
        return std::unexpected(R.error());
    if (auto R = AddRecordRegion(Cursor, Result.RecordStride, BaseAlignment, CallableRecordCount, Result.Callable); !R)
        return std::unexpected(R.error());
    Result.TotalSize = Cursor;
    return Result;
}

/// Vulkan realization of a ray-tracing pipeline and its immutable shader-binding table.
class VulkanRayTracingPipeline final : public RHIRayTracingPipeline {
  public:
    VulkanRayTracingPipeline() = default;

    ~VulkanRayTracingPipeline() override = default;

    VulkanRayTracingPipeline(const VulkanRayTracingPipeline&)                    = delete;
    auto operator=(const VulkanRayTracingPipeline&) -> VulkanRayTracingPipeline& = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext&    Context,
                                     const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<UPtr<VulkanRayTracingPipeline>, ErrorMessage> {
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return std::unexpected(ErrorMessage("Hardware ray tracing is unavailable"));

        const auto& Properties = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
        if (Desc.MaxRecursionDepth == 0 || Desc.MaxRecursionDepth > Properties.maxRayRecursionDepth) {
            return std::unexpected(ErrorMessage(Format(
                "Requested ray recursion depth {} exceeds device limit {}",
                Desc.MaxRecursionDepth,
                Properties.maxRayRecursionDepth)));
        }

        auto LayoutObjects = CreatePipelineLayout(Context.Device, Desc.Program.Reflection, GetRayTracingShaderStageFlags());
        if (!LayoutObjects)
            return std::unexpected(LayoutObjects.error().Append("Failed to create ray-tracing pipeline layout"));

        auto ShaderStates = CreateRayTracingShaderStates(Context.Device, Desc);
        if (!ShaderStates)
            return std::unexpected(ShaderStates.error().Append("Failed to lower ray-tracing shader stages and groups"));

        vk::RayTracingPipelineCreateInfoKHR PipelineCI{
            .stageCount                  = static_cast<Uint32>(ShaderStates->Stages.size()),
            .pStages                     = ShaderStates->Stages.data(),
            .groupCount                  = static_cast<Uint32>(ShaderStates->Groups.size()),
            .pGroups                     = ShaderStates->Groups.data(),
            .maxPipelineRayRecursionDepth = Desc.MaxRecursionDepth,
            .layout                      = *LayoutObjects->second,
        };
        auto [PipelineResult, RHIPipeline] = Context.Device.createRayTracingPipelineKHR(nullptr, nullptr, PipelineCI, nullptr);
        if (PipelineResult != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format(
                "Failed to create ray-tracing pipeline: {}", vk::to_string(PipelineResult))));
        }

        const Uint32 GroupCount = static_cast<Uint32>(ShaderStates->Groups.size());
        const Uint64 HandleDataSize = static_cast<Uint64>(GroupCount) * Properties.shaderGroupHandleSize;
        if (HandleDataSize > std::numeric_limits<std::size_t>::max())
            return std::unexpected(ErrorMessage("Ray-tracing shader group handle query size exceeds host address space"));
        std::vector<Uint8> Handles(HandleDataSize);
        const auto HandleResult = RHIPipeline.getRayTracingShaderGroupHandlesKHR(
            0, GroupCount, Handles.size(), Handles.data());
        if (HandleResult != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format(
                "Failed to query ray-tracing shader group handles: {}", vk::to_string(HandleResult))));
        }

        auto SbtLayout = VulkanCreateShaderBindingTableLayout(Properties.shaderGroupHandleSize,
                                                        Properties.shaderGroupHandleAlignment,
                                                        Properties.shaderGroupBaseAlignment,
                                                        Properties.maxShaderGroupStride,
                                                        1,
                                                        static_cast<Uint32>(ShaderStates->GroupIndices.Miss.size()),
                                                        static_cast<Uint32>(ShaderStates->GroupIndices.Hit.size()),
                                                        static_cast<Uint32>(ShaderStates->GroupIndices.Callable.size()));
        if (!SbtLayout)
            return std::unexpected(SbtLayout.error().Append("Failed to calculate shader-binding-table layout"));

        auto SbtData = PopulateShaderBindingTableData(Handles,
                                                       Properties.shaderGroupHandleSize,
                                                       ShaderStates->GroupIndices,
                                                       *SbtLayout);
        if (!SbtData)
            return std::unexpected(SbtData.error().Append("Failed to populate shader-binding-table records"));

        auto SbtBuffer = VulkanHostBuffer::Create(SbtLayout->TotalSize,
                                            vk::BufferUsageFlagBits::eShaderBindingTableKHR |
                                                vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                            Context.Device,
                                            Context.Allocator);
        if (!SbtBuffer)
            return std::unexpected(SbtBuffer.error().Append("Failed to allocate shader-binding-table buffer"));
        if (auto R = SbtBuffer->Upload(SbtData->data(), SbtData->size()); !R)
            return std::unexpected(R.error().Append("Failed to upload shader-binding-table records"));

        const vk::DeviceAddress SbtAddress =
            SbtBuffer->GetDeviceAddress();
        if (SbtAddress == 0 || SbtAddress % Properties.shaderGroupBaseAlignment != 0) {
            return std::unexpected(ErrorMessage(Format(
                "Shader-binding-table buffer address {} is not aligned to required base alignment {}",
                SbtAddress,
                Properties.shaderGroupBaseAlignment)));
        }

        auto Result = std::make_unique<VulkanRayTracingPipeline>();
        Result->m_Pipeline = std::make_shared<vk::raii::Pipeline>(std::move(RHIPipeline));
        Result->m_SetLayouts =
            std::make_shared<std::vector<vk::raii::DescriptorSetLayout>>(std::move(LayoutObjects->first));
        Result->m_PipelineLayout = std::make_shared<vk::raii::PipelineLayout>(std::move(LayoutObjects->second));
        Result->m_RawSetLayouts.reserve(Result->m_SetLayouts->size());
        for (const auto& SetLayout : *Result->m_SetLayouts)
            Result->m_RawSetLayouts.push_back(*SetLayout);
        Result->m_DescriptorSetCount = static_cast<Uint32>(Result->m_RawSetLayouts.size());
        Result->m_Bindings = BuildReflectedBindings(Desc.Program.Reflection);
        Result->m_DynamicOffsetCount = CountDynamicOffsets(Desc.Program.Reflection);
        Result->m_PushConstantSize = MaxPushConstantSize(Desc.Program.Reflection);
        Result->m_ShaderBindingTable = std::make_shared<VulkanHostBuffer>(std::move(*SbtBuffer));
        Result->m_RayGenerationRegion = MakeRegion(SbtAddress, SbtLayout->RayGeneration);
        Result->m_MissRegion = MakeRegion(SbtAddress, SbtLayout->Miss);
        Result->m_HitRegion = MakeRegion(SbtAddress, SbtLayout->Hit);
        Result->m_CallableRegion = MakeRegion(SbtAddress, SbtLayout->Callable);
        Result->SetShaderParameterLayout(RHIShaderParameterLayout::Create(Desc.Program.Reflection));
        return Result;
    }

    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_Pipeline);
    }

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout {
        return *(*m_PipelineLayout);
    }

    [[nodiscard]] auto GetPushConstantSize() const -> Uint32 {
        return m_PushConstantSize;
    }

    [[nodiscard]] auto GetPushConstantStages() const -> vk::ShaderStageFlags {
        return GetRayTracingShaderStageFlags();
    }

    [[nodiscard]] auto GetOrCreateDescriptorSetInstance(Uint64             ParameterId,
                                                         Uint32             FrameIndex,
                                                         Uint32             SetIndex,
                                                         Uint32             VariableDescriptorCount,
                                                         VulkanDescriptorManager& Descriptors)
        -> std::expected<VulkanDescriptorSetInstance*, ErrorMessage> {
        if (SetIndex >= m_RawSetLayouts.size())
            return std::unexpected(ErrorMessage(Format("Parameter set uses missing descriptor set {}", SetIndex)));

        auto& Frames = m_ParameterSets->ByParameterId[ParameterId];
        if (FrameIndex >= Descriptors.GetFramesInFlight())
            return std::unexpected(ErrorMessage(Format("Invalid frame index {} for descriptor set instance", FrameIndex)));
        if (Frames.size() < Descriptors.GetFramesInFlight())
            Frames.resize(Descriptors.GetFramesInFlight());

        auto& Instances = Frames[FrameIndex];
        if (Instances.size() < m_RawSetLayouts.size())
            Instances.resize(m_RawSetLayouts.size());

        auto& Versions = Instances[SetIndex];
        for (auto& Instance : Versions) {
            if (Instance.VariableDescriptorCount == VariableDescriptorCount)
                return &Instance;
        }

        auto Set = Descriptors.AllocatePersistentDescriptorSet(m_RawSetLayouts[SetIndex], VariableDescriptorCount);
        if (!Set)
            return std::unexpected(Set.error());

        Versions.push_back(VulkanDescriptorSetInstance{
            .Set                     = std::move(*Set),
            .VariableDescriptorCount = VariableDescriptorCount,
        });
        return &Versions.back();
    }

    [[nodiscard]] auto GetRayGenerationRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_RayGenerationRegion;
    }

    [[nodiscard]] auto GetMissRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_MissRegion;
    }

    [[nodiscard]] auto GetHitRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_HitRegion;
    }

    [[nodiscard]] auto GetCallableRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_CallableRegion;
    }

  private:
    SPtr<vk::raii::Pipeline>                         m_Pipeline = nullptr;
    SPtr<vk::raii::PipelineLayout>                   m_PipelineLayout = nullptr;
    SPtr<std::vector<vk::raii::DescriptorSetLayout>> m_SetLayouts = nullptr;
    std::vector<vk::DescriptorSetLayout>             m_RawSetLayouts = {};
    std::vector<VulkanReflectedDescriptorBinding>          m_Bindings = {};
    SPtr<VulkanHostBuffer>                                 m_ShaderBindingTable = nullptr;
    vk::StridedDeviceAddressRegionKHR                m_RayGenerationRegion = {};
    vk::StridedDeviceAddressRegionKHR                m_MissRegion = {};
    vk::StridedDeviceAddressRegionKHR                m_HitRegion = {};
    vk::StridedDeviceAddressRegionKHR                m_CallableRegion = {};
    SPtr<VulkanPipelineParameterSetInstances>              m_ParameterSets = std::make_shared<VulkanPipelineParameterSetInstances>();
    Uint32                                           m_DescriptorSetCount = 0;
    Uint32                                           m_DynamicOffsetCount = 0;
    Uint32                                           m_PushConstantSize = 0;
};

} // namespace SoulEngine
