module;

#include <hlsl++.h>

export module Renderer:RayTracingRenderer;

import Core;
import RHI;
import Resource;
import Scene;

import :IRenderer;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// Constant-buffer ABI mirror for RayTracing.slang camera unprojection data.
struct alignas(16) RayTracingViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjectionInverse = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity =
        hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor =
        hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
};

struct RayTracingGeometryBuffers {
    RHI::VertexBuffer* Position = nullptr;
    RHI::VertexBuffer* Normal = nullptr;
    RHI::IndexBuffer* Index = nullptr;
};

struct RayTracingMeshCacheEntry {
    String                                                   Asset = {};
    Resource::ResourceRef<Resource::Mesh>                   Mesh = {};
    Resource::ResourceRef<Resource::BottomLevelAccelerationStructure> Blas = {};
};

/// @brief Convert a Scene row-vector transform to an RHI TLAS instance transform.
///
/// RHI TLAS transforms are row-major 3x4 matrices with translation in the
/// final column. Scene matrices are row-vector transforms, so the upper-left
/// 3x3 block is transposed while the fourth row becomes translation.
[[nodiscard]] auto ToAccelerationStructureInstanceTransform(const hlslpp::float4x4& Matrix)
    -> RHI::RowMajorTransform3x4 {
    return {
        .M00 = Matrix[0].x, .M01 = Matrix[1].x, .M02 = Matrix[2].x, .M03 = Matrix[3].x,
        .M10 = Matrix[0].y, .M11 = Matrix[1].y, .M12 = Matrix[2].y, .M13 = Matrix[3].y,
        .M20 = Matrix[0].z, .M21 = Matrix[1].z, .M22 = Matrix[2].z, .M23 = Matrix[3].z,
    };
}

/// First SceneSnapshot-driven hardware ray-tracing renderer.
class RayTracingRenderer final : public IRenderer {
  public:
    RayTracingRenderer() = default;
    ~RayTracingRenderer() override { OnDetach(); }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().CurrentApplicationDir() / "Shaders" / "RayTracing.slang";
        auto& Resources = Resource::Manager::Get();
        m_Pipeline = Resources.RequestRayTracingPipelineRef(Resource::RayTracingPipelineRequest{
            .RayGeneration = {.SourcePath = ShaderPath, .EntryPoint = "rayGenMain"},
            .MissEntries = {{.SourcePath = ShaderPath, .EntryPoint = "missMain"}},
            .HitGroups = {{.Type = Shader::RayTracingHitGroupType::Triangles,
                           .ClosestHit = ShaderCompiler::ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain"}}},
        });
        m_Tlas = Resources.RequestTopLevelAccelerationStructureRef(
            "ray_tracing_renderer_main", {.InitialInstanceCapacity = 16,
                                            .BuildFlags = RHI::AccelerationStructureBuildFlags::AllowUpdate});
        m_ViewConstants = Resources.RequestConstantBufferRef(
            "ray_tracing_renderer_view_constants", {.Size = sizeof(RayTracingViewConstants)});
        if (!m_Pipeline || !m_Tlas || !m_ViewConstants)
            return std::unexpected(ErrorMessage("RayTracingRenderer resource request failed"));
        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline = {};
        m_Tlas = {};
        m_ViewConstants = {};
        m_Output = {};
        m_OutputKey = {};
        m_LoggedFirstTrace = false;
        m_MeshCache.clear();
        m_Parameters = {};
    }

    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto& Resources = Resource::Manager::Get();
        auto* Pipeline = Resources.TryGetReady(m_Pipeline);
        auto* Tlas = Resources.TryGetReady(m_Tlas);
        if (!Pipeline || !Tlas)
            return Result;

        std::vector<RHI::AccelerationStructureInstance> Instances;
        Instances.reserve(Scene.Renderables.size());
        for (const auto& Renderable : Scene.Renderables) {
            if (Renderable.MeshAsset.empty())
                continue;
            auto* Blas = Resources.TryGetReady(GetOrRequestBlas(Renderable.MeshAsset));
            if (!Blas || !Blas->GetRhiPayload())
                continue;
            Instances.push_back(RHI::AccelerationStructureInstance{
                .BottomLevelPtr = Blas->GetRhiPayload(),
                .Transform = ToAccelerationStructureInstanceTransform(Renderable.WorldTransform),
            });
        }
        if (Instances.empty())
            return Result;

        const auto& View = Scene.Views.front();
        auto* ViewOutput = Resources.TryGetReady(View.ColorRT);
        if (!ViewOutput)
            return Result;
        EnsureOutputTarget(ViewOutput->GetWidth(), ViewOutput->GetHeight());
        auto* Output = Resources.TryGetReady(m_Output);
        auto* ViewCB = Resources.TryGetReady(m_ViewConstants);
        if (!Output || !ViewCB)
            return Result;
        const auto Geometry = GetReadyGeometry(Resources);
        if (!Geometry)
            return Result;

        if (m_Parameters.GetLayoutId() != Pipeline->GetShaderParameterLayout().GetId())
            m_Parameters = RHI::ShaderParameters::Create(*Pipeline);
        if (auto R = m_Parameters.SetTopLevelAccelerationStructure("g_tlas", Tlas->GetRhiPayload()); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer TLAS parameter binding failed"));
        if (auto R = m_Parameters.SetStorageRenderTarget("g_output", Output); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer output parameter binding failed"));
        if (auto R = m_Parameters.SetStorageVertexBuffer("g_rayTracingGeometry.positions", Geometry->Position); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer position buffer binding failed"));
        if (auto R = m_Parameters.SetStorageVertexBuffer("g_rayTracingGeometry.normals", Geometry->Normal); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer normal buffer binding failed"));
        if (auto R = m_Parameters.SetStorageIndexBuffer("g_rayTracingGeometry.indices", Geometry->Index); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer index buffer binding failed"));
        const auto ViewData = BuildViewConstants(View);
        if (auto R = m_Parameters.SetConstantBuffer("g_rayTracingView.view", ViewCB, &ViewData, sizeof(ViewData)); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer view parameter binding failed"));

        RHI::NonRenderingPass Pass = {};
        Pass.BuildOrUpdateTopLevelAccelerationStructure(Tlas->GetRhiPayload(), Instances);
        Pass.SetRayTracingPipeline(Pipeline);
        Pass.BindShaderParameters(Pipeline, m_Parameters);
        Pass.TraceRays(Pipeline, Output->GetWidth(), Output->GetHeight());
        Result.CmdList.Scopes.push_back(std::move(Pass));
        Result.CmdList.PresentSource = Output;
        if (!m_LoggedFirstTrace) {
            LogInfo("RayTracingRenderer emitted TLAS build + TraceRays for {} instances at {}x{}",
                    Instances.size(), Output->GetWidth(), Output->GetHeight());
            m_LoggedFirstTrace = true;
        }
        return Result;
    }

  private:
    [[nodiscard]] auto GetOrRequestBlas(StringView Asset) -> Resource::ResourceRef<Resource::BottomLevelAccelerationStructure>& {
        for (auto& Entry : m_MeshCache) {
            if (Entry.Asset == Asset)
                return Entry.Blas;
        }

        auto& Entry = m_MeshCache.emplace_back(RayTracingMeshCacheEntry{
            .Asset = String(Asset),
            .Mesh = Resource::Manager::Get().RequestMeshRef(ResolveMeshPath(Asset).string()),
        });
        Entry.Blas = Resource::Manager::Get().RequestBottomLevelAccelerationStructureRef(Entry.Mesh);
        return Entry.Blas;
    }

    [[nodiscard]] auto GetReadyGeometry(Resource::Manager& Resources) const -> std::optional<RayTracingGeometryBuffers> {
        for (const auto& Entry : m_MeshCache) {
            auto* Mesh = Resources.TryGetReady(Entry.Mesh);
            if (!Mesh)
                continue;
            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    auto* Position = Resources.TryGetReady(SubMesh.PositionVB);
                    auto* Normal = Resources.TryGetReady(SubMesh.NormalVB);
                    auto* Index = Resources.TryGetReady(SubMesh.IB);
                    if (Position && Normal && Index)
                        return RayTracingGeometryBuffers{.Position = Position, .Normal = Normal, .Index = Index};
                }
            }
        }
        return std::nullopt;
    }

    auto EnsureOutputTarget(Uint32 Width, Uint32 Height) -> void {
        const auto Key = Format("ray_tracing_output_{}x{}", Width, Height);
        if (m_Output && m_OutputKey == Key)
            return;

        m_Output = Resource::Manager::Get().RequestRenderTargetRef(
            Key,
            RHI::RenderTargetDesc{
                .Width = Width,
                .Height = Height,
                .Format = RHI::Format::B8G8R8A8_UNORM,
                .Usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderStorage |
                         RHI::TextureUsage::FrameOutput,
            });
        m_OutputKey = Key;
    }

    [[nodiscard]] static auto ResolveMeshPath(StringView AssetPath) -> Path {
        const Path Candidate{String(AssetPath)};
        if (Candidate.is_absolute())
            return Candidate;
        return ConfigManager::Get().EngineDirPath().parent_path() / Candidate;
    }

    [[nodiscard]] static auto BuildViewConstants(const Scene::RenderViewSnapshot& View) -> RayTracingViewConstants {
        return RayTracingViewConstants{
            .ViewProjectionInverse = hlslpp::inverse(View.ViewProjection),
            .CameraPosition = hlslpp::interop::float4{
                hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    Resource::ResourceRef<RHI::RayTracingPipeline> m_Pipeline = {};
    Resource::ResourceRef<Resource::TopLevelAccelerationStructure> m_Tlas = {};
    Resource::ResourceRef<RHI::ConstantBuffer> m_ViewConstants = {};
    Resource::ResourceRef<RHI::RenderTarget> m_Output = {};
    String m_OutputKey = {};
    std::vector<RayTracingMeshCacheEntry> m_MeshCache = {};
    RHI::ShaderParameters m_Parameters = {};
    bool m_LoggedFirstTrace = false;
};

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine::Renderer
