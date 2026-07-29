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

import :IRenderer;

export import std;

export namespace SoulEngine {

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) ForwardFrameConstants {
    Float32                         Time = 0.0f;
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity =
        hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor =
        hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
};
static_assert(sizeof(ForwardFrameConstants) == 48, "ForwardFrameConstants must match ForwardPbr.slang ForwardFrameData std140 layout");
static_assert(offsetof(ForwardFrameConstants, Time) == 0, "ForwardFrameConstants::Time must match ForwardFrameData.time");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightDirectionIntensity) == 16,
              "ForwardFrameConstants::DirectionalLightDirectionIntensity must match ForwardFrameData");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightColor) == 32,
              "ForwardFrameConstants::DirectionalLightColor must match ForwardFrameData");

/// @brief Constant buffer layout matching ForwardPbr.slang ViewData.
struct alignas(16) ForwardViewConstants {
    alignas(16) hlslpp::float4x4         ViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
};
static_assert(sizeof(ForwardViewConstants) == 80, "ForwardViewConstants must match ForwardPbr.slang ForwardViewData std140 layout");

/// @brief Storage-buffer layout matching ForwardPbr.slang InstanceData.
struct alignas(16) InstanceData {
    alignas(16) hlslpp::float4x4         WorldTransform = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 BoundingSphere =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    Uint32 MaterialIndex = 0;
};
static_assert(sizeof(InstanceData) == 96, "InstanceData must match ForwardPbr.slang storage-buffer layout");
static_assert(offsetof(InstanceData, WorldTransform) == 0);
static_assert(offsetof(InstanceData, BoundingSphere) == 64);
static_assert(offsetof(InstanceData, MaterialIndex) == 80);

/// @brief Storage-buffer layout matching ForwardPbr.slang ForwardMaterialData.
struct alignas(16) ForwardMaterialData {
    alignas(16) hlslpp::interop::float4 BaseColorFactor =
        hlslpp::interop::float4{hlslpp::float4{0.62f, 0.28f, 0.10f, 1.0f}};
    alignas(16) hlslpp::interop::float4 EmissiveFactor =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    Float32 MetallicFactor = 0.0f;
    Float32 RoughnessFactor = 0.42f;
    /// Index into the g_textures.uTextures shader array; negative disables sampling.
    Int32 BaseColorTextureIndex = -1;
    Int32 NormalTextureIndex = -1;
    Int32 MetallicRoughnessTextureIndex = -1;
    Int32 MetallicTextureIndex = -1;
    Int32 RoughnessTextureIndex = -1;
    Int32 OcclusionTextureIndex = -1;
    Int32 EmissiveTextureIndex = -1;
};
static_assert(sizeof(ForwardMaterialData) == 80, "ForwardMaterialData must match ForwardPbr.slang storage-buffer layout");
static_assert(offsetof(ForwardMaterialData, BaseColorFactor) == 0);
static_assert(offsetof(ForwardMaterialData, EmissiveFactor) == 16);
static_assert(offsetof(ForwardMaterialData, MetallicFactor) == 32);
static_assert(offsetof(ForwardMaterialData, BaseColorTextureIndex) == 40);
static_assert(offsetof(ForwardMaterialData, EmissiveTextureIndex) == 64);

struct ForwardViewParameterState {
    String              ViewRenderTargetKey = {};
    RHIShaderParameters Parameters          = {};
};

/// @brief One concrete indexed draw consumed by the forward raster pass.
struct ForwardDrawInstance {
    ResourceHandle<RHIVertexBuffer>   PositionVB       = {};
    ResourceHandle<RHIVertexBuffer>   NormalVB         = {};
    ResourceHandle<RHIVertexBuffer> TangentVB = {};
    ResourceHandle<RHIVertexBuffer> UVVB = {};
    ResourceHandle<RHIIndexBuffer> IndexBuffer = {};
    PbrMetallicRoughnessMaterial Material = {};
    bool HasUV0 = false;
    bool HasTangents = false;
    hlslpp::float4x4                     WorldTransform  = hlslpp::float4x4::identity();
    hlslpp::interop::float4              BoundingSphere  =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
};

struct ForwardMeshCacheEntry {
    String                                Asset = {};
    ResourceRef<ResourceMesh> Mesh  = {};
};

struct ForwardTextureCacheEntry {
    String                                     Asset   = {};
    ResourceRef<RHISampledTexture> Texture = {};
};

struct ResolvedForwardDraw {
    RHIVertexBuffer*    PositionVB       = nullptr;
    RHIVertexBuffer*    NormalVB         = nullptr;
    RHIVertexBuffer* TangentVB = nullptr;
    RHIVertexBuffer* UVVB = nullptr;
    RHIIndexBuffer* IndexBuffer = nullptr;
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

        m_Pipeline = ResourceManager::Get().RequestGraphicsPipelineRef(GraphicsPipelineRequest{
            .VertEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "vertMain",
            },
            .FragEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "fragMain",
            },
            .VertexInputLayout = MakeVertexInputLayout(),
            .DepthFormat       = RHIFormat::D32_SFLOAT,
        });
        if (!m_Pipeline)
            return std::unexpected(ErrorMessage("Forward PBR graphics pipeline request failed"));

        auto& Resources = ResourceManager::Get();
        m_SamplerLinear = Resources.RequestSamplerRef({.Profile = RHISamplerProfile::LinearRepeat});
        if (!m_SamplerLinear)
            return std::unexpected(ErrorMessage("Forward PBR linear sampler request failed"));

        m_SamplerAniso = Resources.RequestSamplerRef({.Profile = RHISamplerProfile::AnisotropicRepeat});
        if (!m_SamplerAniso)
            return std::unexpected(ErrorMessage("Forward PBR anisotropic sampler request failed"));

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline       = {};
        m_SamplerLinear  = {};
        m_SamplerAniso   = {};
        m_MeshCache.clear();
        m_TextureCache.clear();
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        const auto DrawInstances = BuildDrawInstances(Scene);
        const auto FrameData = BuildFrameConstants(Scene.Time);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, DrawInstances, View, FrameData); !R)
                return std::unexpected(R.error().Append("Forward PBR view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] static auto MakeVertexInputLayout() -> RHIVertexInputLayoutDesc {
        return RHIVertexInputLayoutDesc{
            .Bindings = {
                {.Binding = 0, .Stride = sizeof(hlslpp::interop::float3)},
                {.Binding = 1, .Stride = sizeof(hlslpp::interop::float3)},
                {.Binding = 2, .Stride = sizeof(hlslpp::interop::float4)},
                {.Binding = 3, .Stride = sizeof(hlslpp::interop::float2)},
            },
            .Attributes = {
                {.Location = 0, .Binding = 0, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                {.Location = 1, .Binding = 1, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                {.Location = 2, .Binding = 2, .Format = RHIFormat::R32G32B32A32_SFLOAT, .Offset = 0},
                {.Location = 3, .Binding = 3, .Format = RHIFormat::R32G32_SFLOAT, .Offset = 0},
            },
        };
    }

    [[nodiscard]] auto RenderView(RHICommandList&                    CmdList,
                                  std::span<const ForwardDrawInstance> DrawInstances,
                                  const RenderViewSnapshot&    View,
                                  const ForwardFrameConstants&        FrameData)
        -> std::expected<void, ErrorMessage> {
        auto& Resources = ResourceManager::Get();

        auto* ColorRT = Resources.TryGetReady(View.ColorRT);
        auto* DepthRT = Resources.TryGetReady(View.DepthRT);
        auto* Pipeline = Resources.TryGetReady(m_Pipeline);
        auto* SamplerLinear = Resources.TryGetReady(m_SamplerLinear);
        auto* SamplerAniso = Resources.TryGetReady(m_SamplerAniso);
        if (!ColorRT || !DepthRT || !Pipeline || !SamplerLinear || !SamplerAniso) {
            return {};
        }

        auto FrameBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(FrameData));
        if (!FrameBuffer)
            return std::unexpected(FrameBuffer.error().Append("Forward PBR frame transient constant allocation failed"));

        const auto ViewData = BuildViewConstants(View);
        auto ViewBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(ViewData));
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
            .Desc = RHIRenderingDesc{
                .ColorAttachment = {
                    .TexturePtr = ColorRT,
                    .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f},
                },
                .DepthAttachment = RHIDepthAttachmentDesc{
                    .TexturePtr = DepthRT,
                    .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                },
            },
        };
        if (auto R = Pass.WriteTransientConstantBuffer(*FrameBuffer, std::as_bytes(std::span{&FrameData, 1})); !R)
            return std::unexpected(R.error().Append("Forward PBR frame transient constant write failed"));
        if (auto R = Pass.WriteTransientConstantBuffer(*ViewBuffer, std::as_bytes(std::span{&ViewData, 1})); !R)
            return std::unexpected(R.error().Append("Forward PBR view transient constant write failed"));
        if (!CmdList.PresentSource)
            CmdList.PresentSource = ColorRT;

        std::vector<ResolvedForwardDraw> ResolvedDraws = {};
        std::vector<InstanceData>         Instances     = {};
        std::vector<ForwardMaterialData>  MaterialData  = {};
        std::vector<RHISampledTexture*>   TextureTable  = {};
        std::map<String, Int32, std::less<>> TextureIndices = {};
        const auto ResolveTexture = [&](const String& Asset, bool bEnabled) -> Int32 {
            if (!bEnabled || Asset.empty())
                return -1;
            if (const auto Existing = TextureIndices.find(Asset); Existing != TextureIndices.end())
                return Existing->second;
            auto* Texture = Resources.TryGetReady(GetOrRequestTexture(Asset).GetHandle());
            if (!Texture)
                return -1;
            if (TextureTable.size() >= static_cast<std::size_t>(std::numeric_limits<Int32>::max()))
                return -1;
            const auto Index = static_cast<Int32>(TextureTable.size());
            TextureIndices.emplace(Asset, Index);
            TextureTable.emplace_back(Texture);
            return Index;
        };
        ResolvedDraws.reserve(DrawInstances.size());
        Instances.reserve(DrawInstances.size());
        MaterialData.reserve(DrawInstances.size());

        for (const auto& Instance : DrawInstances) {
            auto* PositionVB = Resources.TryGetReady(Instance.PositionVB);
            auto* NormalVB   = Resources.TryGetReady(Instance.NormalVB);
            auto* IB         = Resources.TryGetReady(Instance.IndexBuffer);
            if (!PositionVB || !NormalVB || !IB)
                continue;

            // Meshes without UVs reuse the position stream as a placeholder binding;
            // the shader only reads UVs when a base-color texture is bound.
            auto* TangentVB = Resources.TryGetReady(Instance.TangentVB);
            auto* UVVB = Instance.UVVB.IsValid() ? Resources.TryGetReady(Instance.UVVB) : nullptr;
            if (!UVVB)
                UVVB = PositionVB;
            if (!TangentVB)
                continue;

            const bool bCanSampleUV0 = Instance.HasUV0;
            const bool bCanSampleNormal = bCanSampleUV0 && Instance.HasTangents;
            const auto MaterialIndex = static_cast<Uint32>(MaterialData.size());
            MaterialData.emplace_back(BuildMaterialData(
                Instance.Material,
                ResolveTexture(Instance.Material.BaseColorTexture, bCanSampleUV0),
                ResolveTexture(Instance.Material.NormalTexture, bCanSampleNormal),
                ResolveTexture(Instance.Material.MetallicRoughnessTexture, bCanSampleUV0),
                ResolveTexture(Instance.Material.MetallicTexture, bCanSampleUV0),
                ResolveTexture(Instance.Material.RoughnessTexture, bCanSampleUV0),
                ResolveTexture(Instance.Material.OcclusionTexture, bCanSampleUV0),
                ResolveTexture(Instance.Material.EmissiveTexture, bCanSampleUV0)));

            Instances.emplace_back(BuildInstanceData(Instance, MaterialIndex));
            ResolvedDraws.emplace_back(ResolvedForwardDraw{
                .PositionVB = PositionVB,
                .NormalVB = NormalVB,
                .TangentVB = TangentVB,
                .UVVB = UVVB,
                .IndexBuffer = IB,
            });
        }

        RHIResourceArray<RHISampledTexture> Textures = {};
        if (TextureTable.empty()) {
            // Vulkan runtime descriptor arrays must carry one element even when no material samples a texture.
            Textures.Set(0, nullptr);
        } else {
            for (std::size_t TextureIndex = 0; TextureIndex < TextureTable.size(); ++TextureIndex)
                Textures.Set(static_cast<Uint32>(TextureIndex), TextureTable[TextureIndex]);
        }
        if (auto R = Parameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("Forward PBR texture parameter binding failed"));

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(Pipeline);
        if (!Instances.empty()) {
            const auto InstanceDataBytes = std::as_bytes(std::span{Instances});
            auto InstanceDataBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(InstanceDataBytes.size_bytes());
            if (!InstanceDataBuffer)
                return std::unexpected(InstanceDataBuffer.error().Append("Forward PBR instance-data transient buffer allocation failed"));
            if (auto R = Pass.WriteTransientShaderStorageBuffer(*InstanceDataBuffer, InstanceDataBytes); !R)
                return std::unexpected(R.error().Append("Forward PBR instance-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_forwardDraw.instances", *InstanceDataBuffer); !R)
                return std::unexpected(R.error().Append("Forward PBR instance-data storage buffer binding failed"));

            const auto MaterialDataBytes = std::as_bytes(std::span{MaterialData});
            auto MaterialDataBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(MaterialDataBytes.size_bytes());
            if (!MaterialDataBuffer)
                return std::unexpected(MaterialDataBuffer.error().Append("Forward PBR material-data transient buffer allocation failed"));
            if (auto R = Pass.WriteTransientShaderStorageBuffer(*MaterialDataBuffer, MaterialDataBytes); !R)
                return std::unexpected(R.error().Append("Forward PBR material-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_forwardDraw.materials", *MaterialDataBuffer); !R)
                return std::unexpected(R.error().Append("Forward PBR material-data storage buffer binding failed"));
        }
        if (!ResolvedDraws.empty()) {
            auto DrawParameters = Parameters;
            Pass.BindShaderParameters(Pipeline, std::move(DrawParameters));
        }
        for (Uint32 InstanceIndex = 0; InstanceIndex < ResolvedDraws.size(); ++InstanceIndex) {
            const auto& Draw = ResolvedDraws[InstanceIndex];
            Pass.PushConstants(Pipeline, 0, &InstanceIndex, sizeof(InstanceIndex));
            Pass.DrawIndexed(Pipeline,
                             std::array<RHIVertexBuffer*, kMaxVertexBufferBindings>{Draw.PositionVB, Draw.NormalVB, Draw.TangentVB, Draw.UVVB},
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

    [[nodiscard]] auto GetOrRequestTexture(StringView Asset) -> ResourceRef<RHISampledTexture>& {
        for (auto& Entry : m_TextureCache) {
            if (Entry.Asset == Asset)
                return Entry.Texture;
        }

        auto& Entry = m_TextureCache.emplace_back(ForwardTextureCacheEntry{
            .Asset   = String(Asset),
            .Texture = ResourceManager::Get().RequestSampledTextureRef(Asset),
        });
        return Entry.Texture;
    }

    [[nodiscard]] auto BuildDrawInstances(const SceneSnapshot& Scene)
        -> std::vector<ForwardDrawInstance> {
        std::vector<ForwardDrawInstance> DrawInstances = {};
        auto& Resources = ResourceManager::Get();

        for (const auto& Renderable : Scene.Renderables) {
            if (Renderable.MeshAsset.empty())
                continue;

            const auto* Mesh = Resources.TryGetReady(GetOrRequestMesh(Renderable.MeshAsset));
            if (!Mesh)
                continue;

            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    if (!SubMesh.PositionVB.IsValid() || !SubMesh.NormalVB.IsValid() || !SubMesh.IB.IsValid())
                        continue;

                    DrawInstances.emplace_back(ForwardDrawInstance{
                        .PositionVB = SubMesh.PositionVB,
                        .NormalVB = SubMesh.NormalVB,
                        .TangentVB = SubMesh.TangentVB,
                        .UVVB = SubMesh.UVVB,
                        .IndexBuffer = SubMesh.IB,
                        .Material = Renderable.MaterialId.empty() && Mesh->GetImportedMaterial(SubMesh.MaterialSlot)
                            ? *Mesh->GetImportedMaterial(SubMesh.MaterialSlot)
                            : Renderable.Material,
                        .HasUV0 = SubMesh.HasUV0,
                        .HasTangents = SubMesh.HasTangents,
                        .WorldTransform  = Renderable.WorldTransform,
                        .BoundingSphere  = BuildWorldBoundingSphere(SubMesh.Positions, Renderable.WorldTransform),
                    });
                }
            }
        }

        return DrawInstances;
    }

    [[nodiscard]] static auto BuildFrameConstants(float Time) -> ForwardFrameConstants {
        return ForwardFrameConstants{
            .Time = Time,
            .DirectionalLightDirectionIntensity =
                hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}},
            .DirectionalLightColor = hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}},
        };
    }

    [[nodiscard]] static auto BuildWorldBoundingSphere(const std::vector<hlslpp::interop::float3>& Positions,
                                                        const hlslpp::float4x4& WorldTransform)
        -> hlslpp::interop::float4 {
        if (Positions.empty())
            return hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

        hlslpp::float3 Min = hlslpp::float3{Positions.front().x, Positions.front().y, Positions.front().z};
        hlslpp::float3 Max = Min;
        for (const auto& Position : Positions) {
            const float PositionX = Position.x;
            const float PositionY = Position.y;
            const float PositionZ = Position.z;
            Min.x = std::min(static_cast<float>(Min.x), PositionX);
            Min.y = std::min(static_cast<float>(Min.y), PositionY);
            Min.z = std::min(static_cast<float>(Min.z), PositionZ);
            Max.x = std::max(static_cast<float>(Max.x), PositionX);
            Max.y = std::max(static_cast<float>(Max.y), PositionY);
            Max.z = std::max(static_cast<float>(Max.z), PositionZ);
        }

        const auto LocalCenter = (Min + Max) * 0.5f;
        float      RadiusSquared = 0.0f;
        for (const auto& Position : Positions) {
            const auto Offset = hlslpp::float3{Position.x, Position.y, Position.z} - LocalCenter;
            RadiusSquared = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
        }

        const auto WorldCenter = hlslpp::mul(hlslpp::float4{LocalCenter.x, LocalCenter.y, LocalCenter.z, 1.0f},
                                             WorldTransform);
        float LinearTransformSquared = 0.0f;
        for (Uint32 Row = 0; Row < 3; ++Row) {
            LinearTransformSquared += WorldTransform[Row].x * WorldTransform[Row].x;
            LinearTransformSquared += WorldTransform[Row].y * WorldTransform[Row].y;
            LinearTransformSquared += WorldTransform[Row].z * WorldTransform[Row].z;
        }

        return hlslpp::interop::float4{hlslpp::float4{WorldCenter.x,
                                                       WorldCenter.y,
                                                       WorldCenter.z,
                                                       std::sqrt(RadiusSquared * LinearTransformSquared)}};
    }

    [[nodiscard]] static auto BuildInstanceData(const ForwardDrawInstance& Instance, Uint32 MaterialIndex)
        -> InstanceData {
        return InstanceData{
            .WorldTransform = Instance.WorldTransform,
            .BoundingSphere = Instance.BoundingSphere,
            .MaterialIndex  = MaterialIndex,
        };
    }

    [[nodiscard]] static auto BuildMaterialData(const PbrMetallicRoughnessMaterial& Material,
                                                Int32 BaseColorTextureIndex,
                                                Int32 NormalTextureIndex,
                                                Int32 MetallicRoughnessTextureIndex,
                                                Int32 MetallicTextureIndex,
                                                Int32 RoughnessTextureIndex,
                                                Int32 OcclusionTextureIndex,
                                                Int32 EmissiveTextureIndex) -> ForwardMaterialData {
        return ForwardMaterialData{
            .BaseColorFactor = hlslpp::interop::float4{hlslpp::float4{Material.BaseColor.x, Material.BaseColor.y, Material.BaseColor.z, 1.0f}},
            .EmissiveFactor = hlslpp::interop::float4{hlslpp::float4{Material.Emissive.x, Material.Emissive.y, Material.Emissive.z, 0.0f}},
            .MetallicFactor = Material.Metallic,
            .RoughnessFactor = Material.Roughness,
            .BaseColorTextureIndex = BaseColorTextureIndex,
            .NormalTextureIndex = NormalTextureIndex,
            .MetallicRoughnessTextureIndex = MetallicRoughnessTextureIndex,
            .MetallicTextureIndex = MetallicTextureIndex,
            .RoughnessTextureIndex = RoughnessTextureIndex,
            .OcclusionTextureIndex = OcclusionTextureIndex,
            .EmissiveTextureIndex = EmissiveTextureIndex,
        };
    }

    [[nodiscard]] static auto BuildViewConstants(const RenderViewSnapshot& View) -> ForwardViewConstants {
        return ForwardViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{
                hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const RenderViewSnapshot& View, const RHIGraphicsPipeline& Pipeline)
        -> RHIShaderParameters& {
        const auto& ViewRenderTargetKey = View.ColorRT.GetKey();
        for (auto& State : m_ViewParameters) {
            if (State.ViewRenderTargetKey != ViewRenderTargetKey)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHIShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(ForwardViewParameterState{
            .ViewRenderTargetKey = ViewRenderTargetKey,
            .Parameters          = RHIShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    ResourceRef<RHIGraphicsPipeline> m_Pipeline       = {};
    ResourceRef<RHISampler>          m_SamplerLinear  = {};
    ResourceRef<RHISampler>          m_SamplerAniso   = {};
    std::vector<ForwardMeshCacheEntry>             m_MeshCache                = {};
    std::vector<ForwardTextureCacheEntry>          m_TextureCache             = {};
    std::vector<ForwardViewParameterState>          m_ViewParameters           = {};
};

RendererFactory::AutoRegistrar<ForwardRenderer> RegForwardRenderer{"Forward"};

} // namespace SoulEngine
