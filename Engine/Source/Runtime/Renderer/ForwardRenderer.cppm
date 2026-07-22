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

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) ForwardFrameConstants {
    Float32                         Time = 0.0f;
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity =
        hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor =
        hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
};
static_assert(sizeof(ForwardFrameConstants) == 48, "ForwardFrameConstants must match ForwardPbr.slang FrameData std140 layout");
static_assert(offsetof(ForwardFrameConstants, Time) == 0, "ForwardFrameConstants::Time must match FrameData.time");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightDirectionIntensity) == 16,
              "ForwardFrameConstants::DirectionalLightDirectionIntensity must match FrameData");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightColor) == 32,
              "ForwardFrameConstants::DirectionalLightColor must match FrameData");

/// @brief Constant buffer layout matching ForwardPbr.slang ViewData.
struct alignas(16) ForwardViewConstants {
    alignas(16) hlslpp::float4x4         ViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
};
static_assert(sizeof(ForwardViewConstants) == 80, "ForwardViewConstants must match ForwardPbr.slang ViewData std140 layout");

/// @brief Constant buffer layout matching ForwardPbr.slang MaterialData.
struct alignas(16) ForwardMaterialConstants {
    alignas(16) hlslpp::interop::float4 BaseColorFactor =
        hlslpp::interop::float4{hlslpp::float4{0.62f, 0.28f, 0.10f, 1.0f}};
    Float32 MetallicFactor  = 0.0f;
    Float32 RoughnessFactor = 0.42f;
};
static_assert(sizeof(ForwardMaterialConstants) == 32,
              "ForwardMaterialConstants must match ForwardPbr.slang MaterialData std140 layout");
static_assert(offsetof(ForwardMaterialConstants, BaseColorFactor) == 0,
              "ForwardMaterialConstants::BaseColorFactor must match MaterialData.baseColorFactor");
static_assert(offsetof(ForwardMaterialConstants, MetallicFactor) == 16,
              "ForwardMaterialConstants::MetallicFactor must match MaterialData.metallicFactor");
static_assert(offsetof(ForwardMaterialConstants, RoughnessFactor) == 20,
              "ForwardMaterialConstants::RoughnessFactor must match MaterialData.roughnessFactor");

/// @brief Constant buffer layout matching ForwardPbr.slang ObjectData.
struct alignas(16) ForwardObjectConstants {
    alignas(16) hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();
};
static_assert(sizeof(ForwardObjectConstants) == 64,
              "ForwardObjectConstants must match ForwardPbr.slang ObjectData std140 layout");
struct ForwardViewParameterState {
    String                ViewConstantBufferKey = {};
    RHI::ShaderParameters Parameters            = {};
};

/// @brief One concrete indexed draw consumed by the forward raster pass.
struct ForwardDrawInstance {
    Resource::ResourceHandle<RHI::VertexBuffer> PositionVB     = {};
    Resource::ResourceHandle<RHI::VertexBuffer> NormalVB       = {};
    Resource::ResourceHandle<RHI::IndexBuffer>  IndexBuffer    = {};
    Material::PbrMetallicRoughnessMaterial          Material       = {};
    hlslpp::float4x4                            WorldTransform = hlslpp::float4x4::identity();
};

struct ForwardMeshCacheEntry {
    String                                Asset = {};
    Resource::ResourceRef<Resource::Mesh> Mesh  = {};
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

        m_Pipeline = Resource::Manager::Get().RequestGraphicsPipelineRef(Resource::GraphicsPipelineRequest{
            .VertEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "vertMain",
            },
            .FragEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "fragMain",
            },
            .VertexInputLayout = MakeVertexInputLayout(),
            .DepthFormat       = RHI::Format::D32_SFLOAT,
        });
        if (!m_Pipeline)
            return std::unexpected(ErrorMessage("Forward PBR graphics pipeline request failed"));

        auto& Resources = Resource::Manager::Get();
        m_FrameConstants = Resources.RequestConstantBufferRef(
            "forward_pbr_frame_constants", {.Size = sizeof(ForwardFrameConstants)});
        if (!m_FrameConstants)
            return std::unexpected(ErrorMessage("Forward PBR frame constant buffer request failed"));

        m_ViewConstants =
            Resources.RequestConstantBufferRef("forward_pbr_view_constants", {.Size = sizeof(ForwardViewConstants)});
        if (!m_ViewConstants)
            return std::unexpected(ErrorMessage("Forward PBR view constant buffer request failed"));

        m_ObjectConstants = Resources.RequestConstantBufferRef("forward_pbr_object_constants", {.Size = sizeof(ForwardObjectConstants)});
        if (!m_ObjectConstants)
            return std::unexpected(ErrorMessage("Forward PBR object constant buffer request failed"));

        m_MaterialConstants = Resources.RequestConstantBufferRef(
            "forward_pbr_material_constants", {.Size = sizeof(ForwardMaterialConstants)});
        if (!m_MaterialConstants)
            return std::unexpected(ErrorMessage("Forward PBR material constant buffer request failed"));

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline                 = {};
        m_FrameConstants           = {};
        m_ViewConstants            = {};
        m_MaterialConstants        = {};
        m_ObjectConstants          = {};
        m_MeshCache.clear();
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto* FrameCB = Resource::Manager::Get().TryGetReady(m_FrameConstants);
        if (!FrameCB)
            return Result;

        const auto DrawInstances = BuildDrawInstances(Scene);
        const auto FrameData = BuildFrameConstants(Scene.Time);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, DrawInstances, View, FrameCB, FrameData); !R)
                return std::unexpected(R.error().Append("Forward PBR view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] static auto MakeVertexInputLayout() -> RHI::VertexInputLayoutDesc {
        return RHI::VertexInputLayoutDesc{
            .Bindings = {
                {.Binding = 0, .Stride = sizeof(hlslpp::interop::float3)},
                {.Binding = 1, .Stride = sizeof(hlslpp::interop::float3)},
            },
            .Attributes = {
                {.Location = 0, .Binding = 0, .Format = RHI::Format::R32G32B32_SFLOAT, .Offset = 0},
                {.Location = 1, .Binding = 1, .Format = RHI::Format::R32G32B32_SFLOAT, .Offset = 0},
            },
        };
    }

    [[nodiscard]] auto RenderView(RHI::CommandList&                    CmdList,
                                  std::span<const ForwardDrawInstance> DrawInstances,
                                  const Scene::RenderViewSnapshot&    View,
                                  RHI::ConstantBuffer*                FrameCB,
                                  const ForwardFrameConstants&        FrameData)
        -> std::expected<void, ErrorMessage> {
        auto& Resources = Resource::Manager::Get();

        auto* ColorRT = Resources.TryGetReady(View.ColorRT);
        auto* DepthRT = Resources.TryGetReady(View.DepthRT);
        auto* ViewCB  = Resources.TryGetReady(m_ViewConstants);
        auto* Pipeline = Resources.TryGetReady(m_Pipeline);
        auto* MaterialCB = Resources.TryGetReady(m_MaterialConstants);
        auto* ObjectCB = Resources.TryGetReady(m_ObjectConstants);
        if (!ColorRT || !DepthRT || !ViewCB || !Pipeline || !MaterialCB || !ObjectCB) {
            return {};
        }

        auto& Parameters = GetViewParameters(View, *Pipeline);
        if (auto R = Parameters.SetConstantBuffer("g_forwardFrameView.frame", FrameCB, &FrameData, sizeof(FrameData)); !R)
            return std::unexpected(R.error().Append("Forward PBR frame parameter binding failed"));

        const auto ViewData = BuildViewConstants(View);
        if (auto R = Parameters.SetConstantBuffer("g_forwardFrameView.view", ViewCB, &ViewData, sizeof(ViewData)); !R)
            return std::unexpected(R.error().Append("Forward PBR view parameter binding failed"));

        RHI::Pass Pass{
            .Desc = RHI::RenderingDesc{
                .ColorAttachment = {
                    .TexturePtr = ColorRT,
                    .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f},
                },
                .DepthAttachment = RHI::DepthAttachmentDesc{
                    .TexturePtr = DepthRT,
                    .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                },
            },
        };
        if (!CmdList.PresentSource)
            CmdList.PresentSource = ColorRT;

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(Pipeline);
        for (const auto& Instance : DrawInstances) {
            auto* PositionVB = Resources.TryGetReady(Instance.PositionVB);
            auto* NormalVB   = Resources.TryGetReady(Instance.NormalVB);
            auto* IB         = Resources.TryGetReady(Instance.IndexBuffer);
            if (!PositionVB || !NormalVB || !IB)
                continue;

            auto DrawParameters = Parameters;
            const auto MaterialData = BuildMaterialConstants(Instance.Material);
            if (auto R = DrawParameters.SetConstantBuffer(
                    "g_forwardMaterial.material", MaterialCB, &MaterialData, sizeof(MaterialData), true);
                !R) {
                return std::unexpected(R.error().Append("Forward PBR material parameter binding failed"));
            }

            const auto ObjectData = BuildObjectConstants(Instance);
            if (auto R = DrawParameters.SetConstantBuffer(
                    "g_forwardObject.object", ObjectCB, &ObjectData, sizeof(ObjectData), true);
                !R) {
                return std::unexpected(R.error().Append("Forward PBR object parameter binding failed"));
            }

            Pass.BindShaderParameters(Pipeline, std::move(DrawParameters));
            Pass.DrawIndexed(Pipeline,
                             std::array<RHI::VertexBuffer*, RHI::kMaxVertexBufferBindings>{PositionVB, NormalVB},
                             IB);
        }

        CmdList.Scopes.push_back(std::move(Pass));
        return {};
    }

    [[nodiscard]] static auto ResolveMeshPath(StringView AssetPath) -> Path {
        const Path Asset{String(AssetPath)};
        if (Asset.is_absolute())
            return Asset.lexically_normal();
        return (ConfigManager::Get().CurrentApplicationDir() / "Assets" / Asset).lexically_normal();
    }

    [[nodiscard]] auto GetOrRequestMesh(StringView Asset) -> Resource::ResourceRef<Resource::Mesh>& {
        for (auto& Entry : m_MeshCache) {
            if (Entry.Asset == Asset)
                return Entry.Mesh;
        }

        auto& Entry = m_MeshCache.emplace_back(ForwardMeshCacheEntry{
            .Asset = String(Asset),
            .Mesh  = Resource::Manager::Get().RequestMeshRef(ResolveMeshPath(Asset).string()),
        });
        return Entry.Mesh;
    }

    [[nodiscard]] auto BuildDrawInstances(const Scene::SceneSnapshot& Scene)
        -> std::vector<ForwardDrawInstance> {
        std::vector<ForwardDrawInstance> DrawInstances = {};
        auto& Resources = Resource::Manager::Get();

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
                        .PositionVB     = SubMesh.PositionVB,
                        .NormalVB       = SubMesh.NormalVB,
                        .IndexBuffer    = SubMesh.IB,
                        .Material       = Renderable.Material,
                        .WorldTransform = Renderable.WorldTransform,
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

    [[nodiscard]] static auto BuildMaterialConstants(const Material::PbrMetallicRoughnessMaterial& Material)
        -> ForwardMaterialConstants {
        return ForwardMaterialConstants{
            .BaseColorFactor = hlslpp::interop::float4{
                hlslpp::float4{Material.BaseColor.x, Material.BaseColor.y, Material.BaseColor.z, 1.0f}},
            .MetallicFactor  = Material.Metallic,
            .RoughnessFactor = Material.Roughness,
        };
    }

    [[nodiscard]] static auto BuildObjectConstants(const ForwardDrawInstance& Instance) -> ForwardObjectConstants {
        return ForwardObjectConstants{.WorldTransform = Instance.WorldTransform};
    }

    [[nodiscard]] static auto BuildViewConstants(const Scene::RenderViewSnapshot& View) -> ForwardViewConstants {
        return ForwardViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{
                hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const Scene::RenderViewSnapshot& View, const RHI::GraphicsPipeline& Pipeline)
        -> RHI::ShaderParameters& {
        const auto& ViewConstantBufferKey = View.ViewCB.GetKey();
        for (auto& State : m_ViewParameters) {
            if (State.ViewConstantBufferKey != ViewConstantBufferKey)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHI::ShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(ForwardViewParameterState{
            .ViewConstantBufferKey = ViewConstantBufferKey,
            .Parameters            = RHI::ShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    Resource::ResourceRef<RHI::GraphicsPipeline> m_Pipeline                 = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_FrameConstants           = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_ViewConstants            = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_MaterialConstants        = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_ObjectConstants          = {};
    std::vector<ForwardMeshCacheEntry>             m_MeshCache                = {};
    std::vector<ForwardViewParameterState>          m_ViewParameters           = {};
};

RendererFactory::AutoRegistrar<ForwardRenderer> RegForwardRenderer{"Forward"};

} // namespace SoulEngine::Renderer
