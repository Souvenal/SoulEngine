export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :RayTracingPipeline;
import :Sampler;
import :Texture;
import :Descriptor;
import :AccelerationStructure;

namespace SoulEngine {

/// Backend-private physical range resolved from a logical transient buffer handle.
struct VulkanTransientBufferSlice {
    Uint64 Offset = 0;
    Uint64 Size   = 0;
};

[[nodiscard]] static auto ToVkClearColor(const RHIClearColorValue& Value) -> vk::ClearColorValue {
    if (Value.UseUInt) {
        vk::ClearColorValue Result = {};
        Result.uint32[0] = Value.UInt[0];
        Result.uint32[1] = Value.UInt[1];
        Result.uint32[2] = Value.UInt[2];
        Result.uint32[3] = Value.UInt[3];
        return Result;
    }
    return vk::ClearColorValue(std::array<float, 4>{Value.R, Value.G, Value.B, Value.A});
}

/// Callable for std::visit over RHICommand variants.
struct VulkanCommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, VulkanImageState>& LocalStates;
    VulkanDescriptorManager*                         Descriptors                = nullptr;
    VulkanTransientUniformArena*                     TransientUniformArena     = nullptr;
    VulkanTransientShaderStorageArena*               TransientShaderStorageArena = nullptr;
    std::vector<std::pair<RHITransientConstantBuffer, VulkanTransientBufferSlice>>* TransientConstantBuffers = nullptr;
    std::vector<std::pair<RHITransientShaderStorageBuffer, VulkanTransientBufferSlice>>*
        TransientShaderStorageBuffers = nullptr;
    std::vector<std::function<void()>>* RetiredPayloads = nullptr;
    Uint32                                     FrameIndex                 = 0;
    vk::Extent2D                               CurrentRenderExtent        = {1, 1};
    std::vector<RHIFormat>                      CurrentColorFormats        = {};
    RHIFormat                                   CurrentDepthFormat         = RHIFormat::Unknown;
    enum class BoundPipelineType : Uint8 {
        Unknown = 0,
        Graphics,
        RayTracing,
    };

    std::optional<ErrorMessage>                Error                      = std::nullopt;
    RHIPipeline*                             m_BoundPipeline            = nullptr;
    BoundPipelineType                          m_BoundPipelineType        = BoundPipelineType::Unknown;

    /// Begin rendering scope from RHIPass desc.
    auto BeginPass(const RHIRenderingDesc& Desc) -> void {
        // ── Resolve color attachment ──────────────────────────────────
        std::vector<vk::RenderingAttachmentInfo> ColorAttachments = {};
        ColorAttachments.reserve(Desc.ColorAttachments.size());
        CurrentColorFormats.clear();
        CurrentColorFormats.reserve(Desc.ColorAttachments.size());
        CurrentDepthFormat = RHIFormat::Unknown;
        Uint32 RenderWidth  = 0;
        Uint32 RenderHeight = 0;

        for (const auto& AttachmentDesc : Desc.ColorAttachments) {
            auto* ColorTarget = AttachmentDesc.TextureRef.TryGet();
            if (!ColorTarget) {
                Error = ErrorMessage("Rendering pass color attachment is not ready");
                return;
            }
            const auto& ColorRT = static_cast<const VulkanRenderTarget&>(*ColorTarget);
            VulkanTransitionImage(Buf,
                                  LocalStates,
                                  ColorRT.GetVkImage(),
                                  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                  vk::AccessFlagBits2::eColorAttachmentWrite,
                                  vk::ImageLayout::eColorAttachmentOptimal,
                                  true,
                                  ToVkImageAspect(ColorRT.GetFormat()));
            ColorAttachments.push_back(vk::RenderingAttachmentInfo{
                .imageView   = ColorRT.GetVkImageView(),
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp      = vk::AttachmentLoadOp::eClear,
                .storeOp     = vk::AttachmentStoreOp::eStore,
                .clearValue  = vk::ClearValue{.color = ToVkClearColor(AttachmentDesc.ClearValue)},
            });
            CurrentColorFormats.push_back(ColorRT.GetFormat());
            RenderWidth  = RenderWidth == 0 ? ColorRT.GetWidth() : (std::min)(RenderWidth, ColorRT.GetWidth());
            RenderHeight = RenderHeight == 0 ? ColorRT.GetHeight() : (std::min)(RenderHeight, ColorRT.GetHeight());
        }
        CurrentRenderExtent = vk::Extent2D{RenderWidth, RenderHeight};

        // ── Resolve depth attachment (optional) ───────────────────────
        std::optional<vk::RenderingAttachmentInfo> DepthAttachment = std::nullopt;
        if (Desc.DepthAttachment.has_value()) {
            auto* DepthTarget = Desc.DepthAttachment->TextureRef.TryGet();
            if (!DepthTarget) {
                Error = ErrorMessage("Rendering pass depth attachment is not ready");
                return;
            }
            auto& VkDepthRT   = static_cast<const VulkanRenderTarget&>(*DepthTarget);
            CurrentDepthFormat = VkDepthRT.GetFormat();
            auto  DepthImage  = VkDepthRT.GetVkImage();
            auto  DepthView   = VkDepthRT.GetVkImageView();
            auto  DepthAspect = ToVkImageAspect(VkDepthRT.GetFormat());
            VulkanTransitionImage(Buf,
                            LocalStates,
                            DepthImage,
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                            vk::ImageLayout::eDepthAttachmentOptimal,
                            true,
                            DepthAspect);
            DepthAttachment = vk::RenderingAttachmentInfo{
                .imageView   = DepthView,
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp      = vk::AttachmentLoadOp::eClear,
                .storeOp     = vk::AttachmentStoreOp::eStore,
                .clearValue  = vk::ClearValue{.depthStencil =
                                                  vk::ClearDepthStencilValue{Desc.DepthAttachment->ClearValue.Depth,
                                                                             Desc.DepthAttachment->ClearValue.Stencil}},
            };
        }

        // Clamp render area to smallest attachment dimension.  Vulkan
        // requires every attachment imageView extent ≥ renderArea, so if
        // the depth RT is smaller than the color RT (or vice versa) we
        // must shrink renderArea to fit the minimum.
        if (Desc.DepthAttachment.has_value()) {
            auto* DepthTarget           = Desc.DepthAttachment->TextureRef.TryGet();
            if (!DepthTarget)
                return;
            auto& VkDepthRT            = static_cast<const VulkanRenderTarget&>(*DepthTarget);
            CurrentRenderExtent.width  = (std::min)(CurrentRenderExtent.width, VkDepthRT.GetWidth());
            CurrentRenderExtent.height = (std::min)(CurrentRenderExtent.height, VkDepthRT.GetHeight());
        }

        // ── Begin dynamic rendering ───────────────────────────────────
        vk::RenderingInfo RenderingInfo{
            .renderArea           = vk::Rect2D{{0, 0}, CurrentRenderExtent},
            .layerCount           = 1,
            .colorAttachmentCount = static_cast<Uint32>(ColorAttachments.size()),
            .pColorAttachments    = ColorAttachments.data(),
            .pDepthAttachment     = DepthAttachment.has_value() ? &*DepthAttachment : nullptr,
        };
        Buf.beginRendering(RenderingInfo);
    }

    /// End rendering scope.
    auto EndPass() -> void {
        Buf.endRendering();
    }

    template <typename PipelineType>
    auto BindShaderParameters(const RHIBindShaderParametersCmd& Cmd,
                              PipelineType&                     Pipeline,
                              vk::PipelineBindPoint             BindPoint,
                              vk::PipelineStageFlags2           ShaderStage) -> void {
        if (Cmd.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId()) {
            Error = ErrorMessage("Shader parameters were created from a different pipeline layout");
            return;
        }

        for (const auto& Parameters : Cmd.Parameters.GetSets()) {
            const auto& Layout   = Parameters.GetLayout();
            const auto  SetIndex = Layout.GetSetIndex();
            const auto  Bindings = Layout.GetBindings();
            const auto  Values   = Parameters.GetValues();

            Uint32 VariableDescriptorCount = 1;
            for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
                const auto& Binding = Bindings[Index];
                if (Binding.ArrayCount != std::numeric_limits<Uint32>::max())
                    continue;
                if (Binding.Type != ShaderResourceType::SampledTexture) {
                    Error = ErrorMessage(Format(
                        "Runtime resource array '{}' uses unsupported resource type",
                        Binding.ParameterPath));
                    return;
                }
                const auto* Array = std::get_if<RHIResourceArray<RHISampledTexture>>(&Values[Index]);
                if (!Array || Array->GetSize() == 0) {
                    Error = ErrorMessage(Format(
                        "Runtime sampled texture array '{}' is missing a non-empty resource array",
                        Binding.ParameterPath));
                    return;
                }
                VariableDescriptorCount = Array->GetSize();
            }

            auto Instance = Pipeline.GetOrCreateDescriptorSetInstance(
                Cmd.Parameters.GetId(), FrameIndex, SetIndex, VariableDescriptorCount, *Descriptors);
            if (!Instance) {
                Error = Instance.error();
                return;
            }

            const bool bUpdateDescriptors = !(*Instance)->Initialized ||
                                            (*Instance)->ParameterRevision != Parameters.GetRevision();
            std::vector<std::pair<Uint32, Uint32>> DynamicOffsetWrites;
            for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
                const auto& Binding = Bindings[Index];
                const auto& Value   = Values[Index];

                if (const auto* TransientConstant = std::get_if<RHITransientConstantBuffer>(&Value)) {
                    if (!TransientConstantBuffers) {
                        Error = ErrorMessage("Transient constant buffer table is unavailable during shader parameter binding");
                        return;
                    }
                    const auto It = std::ranges::find_if(
                        *TransientConstantBuffers,
                        [&TransientConstant](const auto& Entry) -> bool {
                            return Entry.first == *TransientConstant;
                        });
                    if (It == TransientConstantBuffers->end()) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' references an unresolved transient constant buffer",
                            Binding.ParameterPath));
                        return;
                    }
                    if (It->second.Offset > std::numeric_limits<Uint32>::max()) {
                        Error = ErrorMessage("Transient constant buffer offset exceeds dynamic offset range");
                        return;
                    }
                    if (bUpdateDescriptors) {
                        Descriptors->WriteConstantDescriptor(
                            *(*Instance)->Set,
                            Binding.Binding,
                            TransientUniformArena->GetVkBuffer(),
                            It->second.Size);
                    }
                    DynamicOffsetWrites.push_back({Binding.Binding, static_cast<Uint32>(It->second.Offset)});
                    continue;
                }

                if (const auto* Target = std::get_if<RHIRenderTarget*>(&Value)) {
                    if (!*Target) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null render target", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTarget = static_cast<const VulkanRenderTarget&>(**Target);
                    const bool IsSampled = Binding.Type == ShaderResourceType::SampledTexture;
                    VulkanTransitionImage(Buf,
                                    LocalStates,
                                    VkTarget.GetVkImage(),
                                    ShaderStage,
                                    IsSampled ? vk::AccessFlagBits2::eShaderRead
                                              : vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                    IsSampled ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral,
                                    false,
                                    ToVkImageAspect(VkTarget.GetFormat()));
                    if (bUpdateDescriptors) {
                        auto& ResourceBindings = (*Instance)->ResourceBindings;
                        if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Target) {
                            if (IsSampled)
                                Descriptors->WriteSampledTextureDescriptor(
                                    *(*Instance)->Set, Binding.Binding, 0, VkTarget.GetVkImageView(),
                                    vk::ImageLayout::eShaderReadOnlyOptimal);
                            else
                                Descriptors->WriteStorageImageDescriptor(
                                    *(*Instance)->Set, Binding.Binding, VkTarget.GetVkImageView());
                            ResourceBindings[Binding.Binding] = *Target;
                        }
                    }
                    continue;
                }

                if (!bUpdateDescriptors)
                    continue;

                if (const auto* TransientStorage = std::get_if<RHITransientShaderStorageBuffer>(&Value)) {
                    if (!TransientShaderStorageArena || !TransientShaderStorageBuffers) {
                        Error = ErrorMessage("Transient shader storage arena is unavailable during shader parameter binding");
                        return;
                    }
                    const auto It = std::ranges::find_if(
                        *TransientShaderStorageBuffers,
                        [&TransientStorage](const auto& Entry) -> bool {
                            return Entry.first == *TransientStorage;
                        });
                    if (It == TransientShaderStorageBuffers->end()) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' references an unresolved transient storage buffer", Binding.ParameterPath));
                        return;
                    }
                    Descriptors->WriteStorageBufferDescriptor(*(*Instance)->Set,
                                                              Binding.Binding,
                                                              TransientShaderStorageArena->GetVkBuffer(),
                                                              It->second.Size,
                                                              It->second.Offset);
                    continue;
                }

                if (const auto* VertexBuffer = std::get_if<RHIVertexBuffer*>(&Value)) {
                    if (!*VertexBuffer) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null storage vertex buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const VulkanVertexBuffer&>(**VertexBuffer);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *VertexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set,
                            Binding.Binding,
                            VkBuffer.GetVkBuffer(),
                            VkBuffer.GetStride() * VkBuffer.GetVertexCount());
                        ResourceBindings[Binding.Binding] = *VertexBuffer;
                    }
                    continue;
                }

                if (const auto* IndexBuffer = std::get_if<RHIIndexBuffer*>(&Value)) {
                    if (!*IndexBuffer) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null storage index buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const VulkanIndexBuffer&>(**IndexBuffer);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *IndexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkBuffer.GetVkBuffer(), VkBuffer.GetIndexCount() * sizeof(Uint32));
                        ResourceBindings[Binding.Binding] = *IndexBuffer;
                    }
                    continue;
                }

                if (const auto* Texture = std::get_if<RHISampledTexture*>(&Value)) {
                    if (!*Texture) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null sampled texture", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTexture = static_cast<const VulkanSampledTexture&>(**Texture);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Texture) {
                        Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                   Binding.Binding,
                                                                   0,
                                                                   VkTexture.GetVkImageView(),
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
                        ResourceBindings[Binding.Binding] = *Texture;
                    }
                    continue;
                }

                if (const auto* Array = std::get_if<RHIResourceArray<RHISampledTexture>>(&Value)) {
                    if (Array->GetSize() == 0) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has an empty sampled-texture resource array",
                            Binding.ParameterPath));
                        return;
                    }
                    const bool bRuntimeArray = Binding.ArrayCount == std::numeric_limits<Uint32>::max();
                    if (!bRuntimeArray && Array->GetSize() != Binding.ArrayCount) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' sampled-texture array size {} does not match reflected array count {}",
                            Binding.ParameterPath,
                            Array->GetSize(),
                            Binding.ArrayCount));
                        return;
                    }
                    for (Uint32 Slot = 0; Slot < Array->GetResources().size(); ++Slot) {
                        auto* Texture = Array->GetResources()[Slot];
                        if (!Texture && bRuntimeArray)
                            continue;
                        if (!Texture) {
                            Error = ErrorMessage(Format(
                                "Fixed-size shader parameter '{}' has an unset sampled-texture array slot {}",
                                Binding.ParameterPath,
                                Slot));
                            return;
                        }
                        const auto& VkTexture = static_cast<const VulkanSampledTexture&>(*Texture);
                        Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                   Binding.Binding,
                                                                   Slot,
                                                                   VkTexture.GetVkImageView(),
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
                    }
                    continue;
                }

                if (const auto* RHIAccelerationStructure = std::get_if<RHITopLevelAccelerationStructure*>(&Value)) {
                    if (!*RHIAccelerationStructure) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null top-level acceleration structure", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTlas = static_cast<const VulkanTopLevelAccelerationStructure&>(**RHIAccelerationStructure);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *RHIAccelerationStructure) {
                        Descriptors->WriteAccelerationStructureDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkTlas.GetAccelerationStructure());
                        ResourceBindings[Binding.Binding] = *RHIAccelerationStructure;
                    }
                    continue;
                }

                if (const auto* Sampler = std::get_if<RHISampler*>(&Value)) {
                    if (!*Sampler) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null sampler", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkSampler = static_cast<const VulkanSampler&>(**Sampler);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Sampler) {
                        Descriptors->WriteSamplerDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkSampler.GetVkSampler());
                        ResourceBindings[Binding.Binding] = *Sampler;
                    }
                    continue;
                }

                Error = ErrorMessage(Format("Shader parameter '{}' is unset", Binding.ParameterPath));
                return;
            }

            if (bUpdateDescriptors) {
                (*Instance)->ParameterRevision = Parameters.GetRevision();
                (*Instance)->Initialized       = true;
            }

            std::ranges::sort(DynamicOffsetWrites, {}, &std::pair<Uint32, Uint32>::first);
            std::vector<Uint32> DynamicOffsets;
            DynamicOffsets.reserve(DynamicOffsetWrites.size());
            for (const auto& [Binding, Offset] : DynamicOffsetWrites)
                DynamicOffsets.push_back(Offset);

            Buf.bindDescriptorSets(BindPoint,
                                   Pipeline.GetPipelineLayout(),
                                   SetIndex,
                                   std::array{*(*Instance)->Set},
                                   DynamicOffsets);
        }
    }

    auto operator()(const RHISetGraphicsPipelineCmd& Cmd) -> void {
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;

        auto& Pipeline = static_cast<VulkanGraphicsPipeline&>(*PipelinePtr);
        if (!CurrentColorFormats.empty() && !Pipeline.IsCompatibleWith(CurrentColorFormats, CurrentDepthFormat)) {
            Error = ErrorMessage("Graphics pipeline attachment formats do not match the active rendering pass");
            return;
        }
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
        m_BoundPipeline     = PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::Graphics;
    }

    auto operator()(const RHISetRayTracingPipelineCmd& Cmd) -> void {
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        m_BoundPipeline     = PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::RayTracing;
    }

    [[nodiscard]] auto WriteTransientShaderStorageBuffer(RHITransientShaderStorageBuffer Buffer,
                                                          std::span<const std::byte>           Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient shader storage buffer write has an invalid buffer"));
        if (Data.empty())
            return std::unexpected(ErrorMessage("Transient shader storage buffer write has no data"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(ErrorMessage("Transient shader storage buffer write size does not match allocation size"));
        if (!TransientShaderStorageArena || !TransientShaderStorageBuffers)
            return std::unexpected(ErrorMessage("Transient shader storage arena is unavailable during command execution"));
        auto Offset = TransientShaderStorageArena->Allocate(FrameIndex, Data.size_bytes());
        if (!Offset)
            return std::unexpected(Offset.error().Append("Transient shader storage buffer allocation failed"));
        if (auto R = TransientShaderStorageArena->Write(Data.data(), Data.size_bytes(), *Offset); !R)
            return std::unexpected(R.error().Append("Transient shader storage buffer upload failed"));
        if (std::ranges::find_if(
                *TransientShaderStorageBuffers,
                [Buffer](const auto& Entry) -> bool {
                    return Entry.first == Buffer;
                }) != TransientShaderStorageBuffers->end()) {
            return std::unexpected(ErrorMessage("Transient shader storage buffer was written more than once"));
        }
        TransientShaderStorageBuffers->emplace_back(
            Buffer,
            VulkanTransientBufferSlice{
                .Offset = *Offset,
                .Size   = Data.size_bytes(),
            });
        const vk::MemoryBarrier2 Barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        const vk::DependencyInfo Dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &Barrier};
        Buf.pipelineBarrier2(Dependency);
        return {};
    }

    auto operator()(const RHIWriteRayTracingGeometryDataCmd& Cmd) -> void {
        if (Cmd.Instances.empty() || Cmd.Geometries.empty()) {
            Error = ErrorMessage("Ray-tracing geometry data upload has no instances or geometries");
            return;
        }
        for (const auto& Instance : Cmd.Instances) {
            if (Instance.GeometryCount == 0 || Instance.FirstGeometry > Cmd.Geometries.size() ||
                Instance.GeometryCount > Cmd.Geometries.size() - Instance.FirstGeometry) {
                Error = ErrorMessage("Ray-tracing geometry instance has an invalid geometry range");
                return;
            }
        }

        std::vector<RHIRayTracingGeometryData> GeometryData = {};
        GeometryData.reserve(Cmd.Geometries.size());
        for (const auto& Source : Cmd.Geometries) {
            if (!Source.PositionBufferRef.TryGet() || !Source.NormalBufferRef.TryGet() ||
                !Source.TangentBufferRef.TryGet() || !Source.TexCoordBufferRef.TryGet() || !Source.IndexBufferRef.TryGet()) {
                Error = ErrorMessage("Ray-tracing geometry data has a null source buffer");
                return;
            }
            if (Source.PositionStride == 0 || Source.NormalStride == 0 || Source.TangentStride == 0 || Source.TexCoordStride == 0 ||
                Source.IndexStride != sizeof(Uint32) ||
                Source.VertexCount == 0 || Source.IndexCount == 0) {
                Error = ErrorMessage("Ray-tracing geometry data has an unsupported layout");
                return;
            }
            const auto& Position = static_cast<const VulkanVertexBuffer&>(*Source.PositionBufferRef.TryGet());
            const auto& Normal = static_cast<const VulkanVertexBuffer&>(*Source.NormalBufferRef.TryGet());
            const auto& Tangent = static_cast<const VulkanVertexBuffer&>(*Source.TangentBufferRef.TryGet());
            const auto& TexCoord = static_cast<const VulkanVertexBuffer&>(*Source.TexCoordBufferRef.TryGet());
            const auto& Indices = static_cast<const VulkanIndexBuffer&>(*Source.IndexBufferRef.TryGet());
            const auto PositionAddress = Position.GetDeviceAddress();
            const auto NormalAddress = Normal.GetDeviceAddress();
            const auto TangentAddress = Tangent.GetDeviceAddress();
            const auto TexCoordAddress = TexCoord.GetDeviceAddress();
            const auto IndexAddress = Indices.GetDeviceAddress();
            if (PositionAddress == 0 || NormalAddress == 0 || TangentAddress == 0 || TexCoordAddress == 0 || IndexAddress == 0) {
                Error = ErrorMessage("Ray-tracing geometry data has a source buffer without a device address");
                return;
            }
            GeometryData.push_back(RHIRayTracingGeometryData{
                .PositionAddress    = PositionAddress,
                .NormalAddress      = NormalAddress,
                .TangentAddress     = TangentAddress,
                .TexCoordAddress    = TexCoordAddress,
                .IndexAddress       = IndexAddress,
                .PositionByteOffset = Source.PositionByteOffset,
                .NormalByteOffset   = Source.NormalByteOffset,
                .TangentByteOffset  = Source.TangentByteOffset,
                .TexCoordByteOffset = Source.TexCoordByteOffset,
                .IndexByteOffset    = Source.IndexByteOffset,
                .PositionStride     = Source.PositionStride,
                .NormalStride       = Source.NormalStride,
                .TangentStride      = Source.TangentStride,
                .TexCoordStride     = Source.TexCoordStride,
                .IndexStride        = Source.IndexStride,
                .MaterialIndex      = Source.MaterialIndex,
            });
        }

        if (auto R = WriteTransientShaderStorageBuffer(Cmd.InstanceBuffer, std::as_bytes(std::span{Cmd.Instances})); !R) {
            Error = R.error().Append("Ray-tracing instance-data transient storage write failed");
            return;
        }
        if (auto R = WriteTransientShaderStorageBuffer(Cmd.GeometryBuffer, std::as_bytes(std::span{GeometryData})); !R) {
            Error = R.error().Append("Ray-tracing geometry-data transient storage write failed");
            return;
        }
    }

    auto operator()(const RHIWriteTransientConstantBufferCmd& Cmd) -> void {
        if (!Cmd.Buffer.IsValid()) {
            Error = ErrorMessage("Transient constant buffer write has an invalid buffer");
            return;
        }
        if (Cmd.Data.empty()) {
            Error = ErrorMessage("Transient constant buffer write has no data");
            return;
        }
        if (!TransientUniformArena || !TransientConstantBuffers) {
            Error = ErrorMessage("Transient constant arena is unavailable during command execution");
            return;
        }
        auto Offset = TransientUniformArena->Allocate(FrameIndex, Cmd.Data.size());
        if (!Offset) {
            Error = Offset.error().Append("Transient constant buffer allocation failed");
            return;
        }
        if (auto R = TransientUniformArena->Write(Cmd.Data.data(), Cmd.Data.size(), *Offset); !R) {
            Error = R.error().Append("Transient constant buffer upload failed");
            return;
        }
        if (std::ranges::find_if(
                *TransientConstantBuffers,
                [&Cmd](const auto& Entry) -> bool {
                    return Entry.first == Cmd.Buffer;
                }) != TransientConstantBuffers->end()) {
            Error = ErrorMessage("Transient constant buffer was written more than once");
            return;
        }
        TransientConstantBuffers->emplace_back(
            Cmd.Buffer,
            VulkanTransientBufferSlice{
                .Offset = *Offset,
                .Size   = Cmd.Data.size(),
            });
        const vk::MemoryBarrier2 Barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eUniformRead,
        };
        const vk::DependencyInfo Dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &Barrier};
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHIWriteTransientShaderStorageBufferCmd& Cmd) -> void {
        if (auto R = WriteTransientShaderStorageBuffer(Cmd.Buffer, Cmd.Data); !R)
            Error = R.error();
    }

    auto operator()(const RHIBuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        auto* TargetPtr = Cmd.TargetRef.TryGet();
        if (!TargetPtr)
            return;
        auto& Tlas = static_cast<VulkanTopLevelAccelerationStructure&>(*TargetPtr);
        if (auto R = Tlas.RecordBuild(Buf, Cmd.Instances, Cmd.Mode, RetiredPayloads); !R) {
            Error = R.error().Append("Failed to record TLAS build");
            return;
        }

        const vk::MemoryBarrier2 BuildBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        const vk::DependencyInfo Dependency{
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &BuildBarrier,
        };
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHITraceRaysCmd& Cmd) -> void {
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*PipelinePtr);
        Buf.traceRaysKHR(Pipeline.GetRayGenerationRegion(),
                         Pipeline.GetMissRegion(),
                         Pipeline.GetHitRegion(),
                         Pipeline.GetCallableRegion(),
                         Cmd.Width,
                         Cmd.Height,
                         Cmd.Depth);

        const vk::MemoryBarrier2 TraceBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        };
        const vk::DependencyInfo Dependency{
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &TraceBarrier,
        };
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHIPushConstantsCmd& Cmd) -> void {
        RHIPipeline* PipelinePtr = Cmd.PipelineRef.Graphics.TryGet();
        if (!PipelinePtr)
            PipelinePtr = Cmd.PipelineRef.RayTracing.TryGet();
        if (!PipelinePtr || Cmd.Data.empty())
            return;
        if (m_BoundPipeline != PipelinePtr) {
            Error = ErrorMessage("PushConstants pipeline is not currently bound");
            return;
        }

        vk::PipelineLayout PipelineLayout = nullptr;
        vk::ShaderStageFlags Stages = {};
        Uint32 PushConstantSize = 0;
        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics: {
            const auto& Pipeline = static_cast<const VulkanGraphicsPipeline&>(*PipelinePtr);
            PipelineLayout = Pipeline.GetPipelineLayout();
            Stages = vk::ShaderStageFlagBits::eAllGraphics;
            PushConstantSize = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::RayTracing: {
            const auto& Pipeline = static_cast<const VulkanRayTracingPipeline&>(*PipelinePtr);
            PipelineLayout = Pipeline.GetPipelineLayout();
            Stages = Pipeline.GetPushConstantStages();
            PushConstantSize = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("PushConstants has no bound pipeline type");
            return;
        }

        if (Cmd.Offset + Cmd.Data.size() > PushConstantSize) {
            Error = ErrorMessage(Format("PushConstants range exceeds reflected pipeline push-constant size ({} + {} > {})",
                                              Cmd.Offset,
                                              Cmd.Data.size(),
                                              PushConstantSize));
            return;
        }

        Buf.pushConstants(PipelineLayout,
                          Stages,
                          Cmd.Offset,
                          static_cast<Uint32>(Cmd.Data.size()),
                          Cmd.Data.data());
    }

    auto operator()(const RHIBindShaderParametersCmd& Cmd) -> void {
        RHIPipeline* PipelinePtr = Cmd.PipelineRef.Graphics.TryGet();
        if (!PipelinePtr)
            PipelinePtr = Cmd.PipelineRef.RayTracing.TryGet();
        if (!PipelinePtr)
            return;
        if (!Descriptors || !TransientUniformArena || !TransientShaderStorageArena ||
            !TransientConstantBuffers || !TransientShaderStorageBuffers) {
            Error = ErrorMessage("VulkanCommandVisitor: descriptor manager or transient arena is missing");
            return;
        }
        if (m_BoundPipeline != PipelinePtr) {
            Error = ErrorMessage("BindShaderParameters pipeline is not currently bound");
            return;
        }

        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanGraphicsPipeline&>(*PipelinePtr),
                                 vk::PipelineBindPoint::eGraphics,
                                 vk::PipelineStageFlagBits2::eAllGraphics);
            return;
        case BoundPipelineType::RayTracing:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanRayTracingPipeline&>(*PipelinePtr),
                                 vk::PipelineBindPoint::eRayTracingKHR,
                                 vk::PipelineStageFlagBits2::eRayTracingShaderKHR);
            return;
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("BindShaderParameters has no bound pipeline type");
            return;
        }
    }

    auto operator()(const RHISetViewportCmd& Cmd) -> void {
        // Vulkan allows a negative viewport height to flip the framebuffer Y axis.
        vk::Viewport Viewport{
            Cmd.X,
            Cmd.Y + Cmd.Height,
            Cmd.Width,
            -Cmd.Height,
            Cmd.MinDepth,
            Cmd.MaxDepth,
        };
        Buf.setViewport(0, {Viewport});
    }

    auto operator()(const RHISetFullViewportCmd& Cmd) -> void {
        // Vulkan allows a negative viewport height to flip the framebuffer Y axis.
        vk::Viewport Viewport{
            0.0f,
            static_cast<Float32>(CurrentRenderExtent.height),
            static_cast<Float32>(CurrentRenderExtent.width),
            -static_cast<Float32>(CurrentRenderExtent.height),
            Cmd.MinDepth,
            Cmd.MaxDepth,
        };
        Buf.setViewport(0, {Viewport});
    }

    auto operator()(const RHISetScissorCmd& Cmd) -> void {
        Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
    }

    auto operator()(const RHISetFullScissorRectCmd& /*Cmd*/) -> void {
        Buf.setScissor(0, {vk::Rect2D{{0, 0}, CurrentRenderExtent}});
    }

    auto operator()(const RHIDrawIndexedCmd& Cmd) -> void {
        if (!Cmd.PipelineRef.TryGet() || !Cmd.VertexBufferRefs[0].TryGet() || !Cmd.IndexBufferRef.TryGet())
            return;

        for (Uint32 Binding = 0; Binding < Cmd.VertexBufferRefs.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBufferRefs[Binding].TryGet();
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }

        const auto& VkIB = static_cast<const VulkanIndexBuffer&>(*Cmd.IndexBufferRef.TryGet());
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHIDrawCmd& Cmd) -> void {
        if (!Cmd.PipelineRef.TryGet())
            return;
        if (!Cmd.VertexBufferRefs[0].TryGet()) {
            Buf.draw(3, 1, 0, 0);
            return;
        }

        for (Uint32 Binding = 0; Binding < Cmd.VertexBufferRefs.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBufferRefs[Binding].TryGet();
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*Cmd.VertexBufferRefs[0].TryGet());
        Buf.draw(static_cast<Uint32>(VkVB.GetVertexCount()), 1, 0, 0);
    }
};

} // namespace SoulEngine
