module;

// needed for offsetof
#include <cstddef>
#include <hlsl++.h>

export module Renderer:ForwardRenderer;

import Core;
import Material;
import RHI;
import Resource;
import Scene;
import TaskGraph;

import :IRenderer;
import :MaterialResolver;

export import std;

export namespace SoulEngine {

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) ForwardFrameConstants {
    Float32 Time                                                           = 0.0f;
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity = hlslpp::interop::float4{
        hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor = hlslpp::interop::float4{
        hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
};
static_assert(sizeof(ForwardFrameConstants) == 48,
              "ForwardFrameConstants must match ForwardPbr.slang ForwardFrameData std140 layout");
static_assert(offsetof(ForwardFrameConstants, Time) == 0,
              "ForwardFrameConstants::Time must match ForwardFrameData.time");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightDirectionIntensity) == 16,
              "ForwardFrameConstants::DirectionalLightDirectionIntensity must match ForwardFrameData");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightColor) == 32,
              "ForwardFrameConstants::DirectionalLightColor must match ForwardFrameData");

/// @brief Constant buffer layout matching ForwardPbr.slang ViewData.
struct alignas(16) ForwardViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjection        = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
};
static_assert(sizeof(ForwardViewConstants) == 80,
              "ForwardViewConstants must match ForwardPbr.slang ForwardViewData std140 layout");

/// @brief Storage-buffer layout matching ForwardPbr.slang InstanceData.
struct alignas(16) InstanceData {
    alignas(16) hlslpp::float4x4 WorldTransform        = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 BoundingSphere = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    Uint32 MaterialIndex = 0;
};
static_assert(sizeof(InstanceData) == 96, "InstanceData must match ForwardPbr.slang storage-buffer layout");
static_assert(offsetof(InstanceData, WorldTransform) == 0);
static_assert(offsetof(InstanceData, BoundingSphere) == 64);
static_assert(offsetof(InstanceData, MaterialIndex) == 80);

struct ForwardViewParameterState {
    RHIRenderTarget*    ViewRenderTargetPtr = nullptr;
    RHIShaderParameters Parameters          = {};
};

/// @brief One concrete indexed draw consumed by the forward raster pass.
struct ForwardDrawInstance {
    RHIRef<RHIVertexBuffer>      PositionVB     = nullptr;
    RHIRef<RHIVertexBuffer>      NormalVB       = nullptr;
    RHIRef<RHIVertexBuffer>      TangentVB      = nullptr;
    RHIRef<RHIVertexBuffer>      UVVB           = nullptr;
    RHIRef<RHIIndexBuffer>       IndexBuffer    = nullptr;
    PbrMetallicRoughnessMaterial Material       = {};
    bool                         HasUV0         = false;
    bool                         HasTangents    = false;
    hlslpp::float4x4             WorldTransform = hlslpp::float4x4::identity();
    hlslpp::interop::float4      BoundingSphere = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
};

struct ForwardMeshCacheEntry {
    String                    Asset = {};
    ResourceRef<ResourceMesh> Mesh  = {};
};

struct ResolvedForwardDraw {
    RHIRef<RHIVertexBuffer> PositionVB  = nullptr;
    RHIRef<RHIVertexBuffer> NormalVB    = nullptr;
    RHIRef<RHIVertexBuffer> TangentVB   = nullptr;
    RHIRef<RHIVertexBuffer> UVVB        = nullptr;
    RHIRef<RHIIndexBuffer>  IndexBuffer = nullptr;
};

/// @brief Single-material metallic-roughness forward renderer.
class ForwardRenderer final : public IRenderer {
  public:
    ForwardRenderer() = default;
    ~ForwardRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "ForwardPbr.slang";

        auto PipelineRequest = SubmitGraphicsPipelinePreparation(
            GraphicsPipelineRequest{
                .VertEntry =
                    {
                        .SourcePath = ShaderPath,
                        .EntryPoint = "vertMain",
                    },
                .FragEntry =
                    {
                        .SourcePath = ShaderPath,
                        .EntryPoint = "fragMain",
                    },
                .VertexInputLayout = MakeVertexInputLayout(),
                .DepthFormat       = RHIFormat::D32_SFLOAT,
            },
            [this](RHIRef<RHIGraphicsPipeline> Pipeline) { m_Pipeline = std::move(Pipeline); });
        if (!PipelineRequest)
            return std::unexpected(PipelineRequest.error().Append("Forward PBR graphics pipeline request failed"));

        m_SamplerLinear = RequestSampler({.Profile = RHISamplerProfile::LinearRepeat});
        if (!m_SamplerLinear)
            return std::unexpected(ErrorMessage("Forward PBR linear sampler request failed"));

        m_SamplerAniso = RequestSampler({.Profile = RHISamplerProfile::AnisotropicRepeat});
        if (!m_SamplerAniso)
            return std::unexpected(ErrorMessage("Forward PBR anisotropic sampler request failed"));

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline      = {};
        m_SamplerLinear = {};
        m_SamplerAniso  = {};
        m_MeshCache.clear();
        m_MaterialResolver.Clear();
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        const auto DrawInstances = BuildDrawInstances(Scene);
        const auto FrameData     = BuildFrameConstants(Scene.Time);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, DrawInstances, View, FrameData); !R)
                return std::unexpected(R.error().Append("Forward PBR view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] static auto RequestSampler(const RHISamplerDesc& Desc) -> RHIRef<RHISampler> {
        auto Sampler = RHIRenderDevice::Get().CreateSampler(Desc);
        if (!Sampler) {
            LogError("Failed to queue forward renderer sampler creation: {}", Sampler.error().ToString());
            return {};
        }
        return std::move(*Sampler);
    }

    [[nodiscard]] static auto MakeVertexInputLayout() -> RHIVertexInputLayoutDesc {
        return RHIVertexInputLayoutDesc{
            .Bindings =
                {
                    {.Binding = 0, .Stride = sizeof(hlslpp::interop::float3)},
                    {.Binding = 1, .Stride = sizeof(hlslpp::interop::float3)},
                    {.Binding = 2, .Stride = sizeof(hlslpp::interop::float4)},
                    {.Binding = 3, .Stride = sizeof(hlslpp::interop::float2)},
                },
            .Attributes =
                {
                    {.Location = 0, .Binding = 0, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                    {.Location = 1, .Binding = 1, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                    {.Location = 2, .Binding = 2, .Format = RHIFormat::R32G32B32A32_SFLOAT, .Offset = 0},
                    {.Location = 3, .Binding = 3, .Format = RHIFormat::R32G32_SFLOAT, .Offset = 0},
                },
        };
    }

    [[nodiscard]] auto RenderView(RHICommandList&                      CmdList,
                                  std::span<const ForwardDrawInstance> DrawInstances,
                                  const RenderViewSnapshot&            View,
                                  const ForwardFrameConstants&         FrameData) -> std::expected<void, ErrorMessage> {
        auto& Resources = ResourceManager::Get();

        auto  ColorRTRef       = View.ColorRT;
        auto  DepthRTRef       = View.DepthRT;
        auto  PipelineRef      = m_Pipeline;
        auto* ColorRT          = ColorRTRef.TryGet();
        auto* DepthRT          = DepthRTRef.TryGet();
        auto* Pipeline         = PipelineRef.TryGet();
        auto  SamplerLinearRef = m_SamplerLinear;
        auto  SamplerAnisoRef  = m_SamplerAniso;
        auto* SamplerLinear    = SamplerLinearRef.TryGet();
        auto* SamplerAniso     = SamplerAnisoRef.TryGet();
        if (!ColorRT || !DepthRT || !Pipeline || !SamplerLinear || !SamplerAniso) {
            return {};
        }

        auto FrameBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(FrameData));
        if (!FrameBuffer)
            return std::unexpected(
                FrameBuffer.error().Append("Forward PBR frame transient constant allocation failed"));

        const auto ViewData   = BuildViewConstants(View);
        auto       ViewBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(ViewData));
        if (!ViewBuffer)
            return std::unexpected(ViewBuffer.error().Append("Forward PBR view transient constant allocation failed"));

        auto& Parameters = GetViewParameters(View, *Pipeline);
        if (auto R = Parameters.SetTransientConstantBuffer("g_forwardFrameView.frame", *FrameBuffer); !R)
            return std::unexpected(R.error().Append("Forward PBR frame parameter binding failed"));

        if (auto R = Parameters.SetTransientConstantBuffer("g_forwardFrameView.view", *ViewBuffer); !R)
            return std::unexpected(R.error().Append("Forward PBR view parameter binding failed"));
        if (auto R = Parameters.SetSampler("g_samplers.uSamplerLinear", SamplerLinear); !R)
            return std::unexpected(R.error().Append("Forward PBR sampler parameter binding failed"));
        if (auto R = Parameters.SetSampler("g_samplers.uSamplerAniso", SamplerAniso); !R)
            return std::unexpected(R.error().Append("Forward PBR sampler parameter binding failed"));

        RHIPass Pass{
            .Desc =
                RHIRenderingDesc{
                    .ColorAttachment =
                        {
                            .TexturePtr = ColorRT,
                            .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f},
                        },
                    .DepthAttachment =
                        RHIDepthAttachmentDesc{
                            .TexturePtr = DepthRT,
                            .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                        },
                },
        };
        Pass.ColorAttachmentRef = ColorRTRef;
        Pass.DepthAttachmentRef = DepthRTRef;
        if (auto R = Pass.WriteTransientConstantBuffer(*FrameBuffer, std::as_bytes(std::span{&FrameData, 1})); !R)
            return std::unexpected(R.error().Append("Forward PBR frame transient constant write failed"));
        if (auto R = Pass.WriteTransientConstantBuffer(*ViewBuffer, std::as_bytes(std::span{&ViewData, 1})); !R)
            return std::unexpected(R.error().Append("Forward PBR view transient constant write failed"));
        if (!CmdList.PresentSourceRef.IsValid()) {
            CmdList.PresentSourceRef = ColorRTRef;
        }

        std::vector<ResolvedForwardDraw> ResolvedDraws = {};
        std::vector<InstanceData>        Instances     = {};
        m_MaterialResolver.BeginFrame();
        ResolvedDraws.reserve(DrawInstances.size());
        Instances.reserve(DrawInstances.size());

        for (const auto& Instance : DrawInstances) {
            if (!Instance.PositionVB || !Instance.NormalVB || !Instance.TangentVB || !Instance.UVVB ||
                !Instance.IndexBuffer)
                continue;

            const auto MaterialIndex =
                m_MaterialResolver.Resolve(Instance.Material, Instance.HasUV0, Instance.HasTangents);
            Instances.emplace_back(BuildInstanceData(Instance, MaterialIndex));
            ResolvedDraws.emplace_back(ResolvedForwardDraw{
                .PositionVB  = Instance.PositionVB,
                .NormalVB    = Instance.NormalVB,
                .TangentVB   = Instance.TangentVB,
                .UVVB        = Instance.UVVB,
                .IndexBuffer = Instance.IndexBuffer,
            });
        }

        const auto Textures = m_MaterialResolver.BuildTextureArray();
        if (auto R = Parameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("Forward PBR texture parameter binding failed"));

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(PipelineRef);
        if (!Instances.empty()) {
            const auto InstanceDataBytes = std::as_bytes(std::span{Instances});
            auto       InstanceDataBuffer =
                RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(InstanceDataBytes.size_bytes());
            if (!InstanceDataBuffer)
                return std::unexpected(
                    InstanceDataBuffer.error().Append("Forward PBR instance-data transient buffer allocation failed"));
            if (auto R = Pass.WriteTransientShaderStorageBuffer(*InstanceDataBuffer, InstanceDataBytes); !R)
                return std::unexpected(R.error().Append("Forward PBR instance-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_forwardDraw.instances", *InstanceDataBuffer); !R)
                return std::unexpected(R.error().Append("Forward PBR instance-data storage buffer binding failed"));

            const auto MaterialDataBytes = std::as_bytes(m_MaterialResolver.GetMaterials());
            auto       MaterialDataBuffer =
                RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(MaterialDataBytes.size_bytes());
            if (!MaterialDataBuffer)
                return std::unexpected(
                    MaterialDataBuffer.error().Append("Forward PBR material-data transient buffer allocation failed"));
            if (auto R = Pass.WriteTransientShaderStorageBuffer(*MaterialDataBuffer, MaterialDataBytes); !R)
                return std::unexpected(R.error().Append("Forward PBR material-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_forwardDraw.materials", *MaterialDataBuffer); !R)
                return std::unexpected(R.error().Append("Forward PBR material-data storage buffer binding failed"));
        }
        if (!ResolvedDraws.empty()) {
            auto DrawParameters = Parameters;
            Pass.BindShaderParameters(
                PipelineRef,
                std::move(DrawParameters),
                RHIShaderParameterResources{
                    .SampledTextures =
                        std::vector<RHIRef<RHISampledTexture>>{m_MaterialResolver.GetTextureRefs().begin(),
                                                               m_MaterialResolver.GetTextureRefs().end()},
                    .Samplers = {SamplerLinearRef, SamplerAnisoRef},
                });
        }
        for (Uint32 InstanceIndex = 0; InstanceIndex < ResolvedDraws.size(); ++InstanceIndex) {
            const auto& Draw = ResolvedDraws[InstanceIndex];
            Pass.PushConstants(PipelineRef, 0, &InstanceIndex, sizeof(InstanceIndex));
            Pass.DrawIndexed(PipelineRef,
                             std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings>{
                                 Draw.PositionVB, Draw.NormalVB, Draw.TangentVB, Draw.UVVB},
                             Draw.IndexBuffer);
        }

        CmdList.Scopes.push_back(std::move(Pass));
        return {};
    }

    [[nodiscard]] auto GetOrRequestMesh(StringView Asset) -> ResourceRef<ResourceMesh>& {
        for (auto& Entry : m_MeshCache) {
            if (Entry.Asset == Asset)
                return Entry.Mesh;
        }

        auto& Entry = m_MeshCache.emplace_back(ForwardMeshCacheEntry{
            .Asset = String(Asset),
            .Mesh  = ResourceManager::Get().RequestMeshRef(Asset),
        });
        return Entry.Mesh;
    }

    [[nodiscard]] auto BuildDrawInstances(const SceneSnapshot& Scene) -> std::vector<ForwardDrawInstance> {
        std::vector<ForwardDrawInstance> DrawInstances = {};
        auto&                            Resources     = ResourceManager::Get();

        for (const auto& Renderable : Scene.Renderables) {
            if (Renderable.MeshAsset.empty())
                continue;

            const auto* Mesh = Resources.TryGetReady(GetOrRequestMesh(Renderable.MeshAsset));
            if (!Mesh)
                continue;

            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    auto PositionVB  = SubMesh.PositionVB;
                    auto NormalVB    = SubMesh.NormalVB;
                    auto TangentVB   = SubMesh.TangentVB;
                    auto UVVB        = SubMesh.UVVB;
                    auto IndexBuffer = SubMesh.IB;
                    if (!PositionVB.TryGet() || !NormalVB.TryGet() || !TangentVB.TryGet() || !IndexBuffer.TryGet())
                        continue;
                    if (!UVVB.TryGet())
                        UVVB = PositionVB;

                    DrawInstances.emplace_back(ForwardDrawInstance{
                        .PositionVB  = std::move(PositionVB),
                        .NormalVB    = std::move(NormalVB),
                        .TangentVB   = std::move(TangentVB),
                        .UVVB        = std::move(UVVB),
                        .IndexBuffer = std::move(IndexBuffer),
                        .Material    = Renderable.MaterialId.empty() && Mesh->GetImportedMaterial(SubMesh.MaterialSlot)
                                           ? *Mesh->GetImportedMaterial(SubMesh.MaterialSlot)
                                           : Renderable.Material,
                        .HasUV0      = SubMesh.HasUV0,
                        .HasTangents = SubMesh.HasTangents,
                        .WorldTransform = Renderable.WorldTransform,
                        .BoundingSphere = BuildWorldBoundingSphere(SubMesh.Positions, Renderable.WorldTransform),
                    });
                }
            }
        }

        return DrawInstances;
    }

    [[nodiscard]] static auto BuildFrameConstants(float Time) -> ForwardFrameConstants {
        return ForwardFrameConstants{
            .Time                               = Time,
            .DirectionalLightDirectionIntensity = hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}},
            .DirectionalLightColor              = hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}},
        };
    }

    [[nodiscard]] static auto BuildWorldBoundingSphere(const std::vector<hlslpp::interop::float3>& Positions,
                                                       const hlslpp::float4x4&                     WorldTransform)
        -> hlslpp::interop::float4 {
        if (Positions.empty())
            return hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

        hlslpp::float3 Min = hlslpp::float3{Positions.front().x, Positions.front().y, Positions.front().z};
        hlslpp::float3 Max = Min;
        for (const auto& Position : Positions) {
            const float PositionX = Position.x;
            const float PositionY = Position.y;
            const float PositionZ = Position.z;
            Min.x                 = std::min(static_cast<float>(Min.x), PositionX);
            Min.y                 = std::min(static_cast<float>(Min.y), PositionY);
            Min.z                 = std::min(static_cast<float>(Min.z), PositionZ);
            Max.x                 = std::max(static_cast<float>(Max.x), PositionX);
            Max.y                 = std::max(static_cast<float>(Max.y), PositionY);
            Max.z                 = std::max(static_cast<float>(Max.z), PositionZ);
        }

        const auto LocalCenter   = (Min + Max) * 0.5f;
        float      RadiusSquared = 0.0f;
        for (const auto& Position : Positions) {
            const auto Offset = hlslpp::float3{Position.x, Position.y, Position.z} - LocalCenter;
            RadiusSquared     = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
        }

        const auto WorldCenter =
            hlslpp::mul(hlslpp::float4{LocalCenter.x, LocalCenter.y, LocalCenter.z, 1.0f}, WorldTransform);
        float LinearTransformSquared = 0.0f;
        for (Uint32 Row = 0; Row < 3; ++Row) {
            LinearTransformSquared += WorldTransform[Row].x * WorldTransform[Row].x;
            LinearTransformSquared += WorldTransform[Row].y * WorldTransform[Row].y;
            LinearTransformSquared += WorldTransform[Row].z * WorldTransform[Row].z;
        }

        return hlslpp::interop::float4{hlslpp::float4{
            WorldCenter.x, WorldCenter.y, WorldCenter.z, std::sqrt(RadiusSquared * LinearTransformSquared)}};
    }

    [[nodiscard]] static auto BuildInstanceData(const ForwardDrawInstance& Instance, Uint32 MaterialIndex)
        -> InstanceData {
        return InstanceData{
            .WorldTransform = Instance.WorldTransform,
            .BoundingSphere = Instance.BoundingSphere,
            .MaterialIndex  = MaterialIndex,
        };
    }

    [[nodiscard]] static auto BuildViewConstants(const RenderViewSnapshot& View) -> ForwardViewConstants {
        return ForwardViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const RenderViewSnapshot& View, const RHIGraphicsPipeline& Pipeline)
        -> RHIShaderParameters& {
        auto* ViewRenderTargetPtr = View.ColorRT.TryGet();
        for (auto& State : m_ViewParameters) {
            if (State.ViewRenderTargetPtr != ViewRenderTargetPtr)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHIShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(ForwardViewParameterState{
            .ViewRenderTargetPtr = ViewRenderTargetPtr,
            .Parameters          = RHIShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    RHIRef<RHIGraphicsPipeline>            m_Pipeline         = nullptr;
    RHIRef<RHISampler>                     m_SamplerLinear    = nullptr;
    RHIRef<RHISampler>                     m_SamplerAniso     = nullptr;
    std::vector<ForwardMeshCacheEntry>     m_MeshCache        = {};
    PbrMaterialResolver                    m_MaterialResolver = {};
    std::vector<ForwardViewParameterState> m_ViewParameters   = {};
};

RendererFactory::AutoRegistrar<ForwardRenderer> RegForwardRenderer{"Forward"};

} // namespace SoulEngine
