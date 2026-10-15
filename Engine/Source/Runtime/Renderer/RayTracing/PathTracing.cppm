export module Renderer:RayTracing.PathTracing;

import Core;
import RHI;
import RenderGraph;

import std;
import ShaderCompiler;
import Shader;

export namespace SoulEngine {

/// @brief Trace dimensions for one frame (CPU passthrough, declared as one
/// aggregate field to stay within the Parameter arity cap).
struct PathTracingExtent {
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief The path-tracing pass: binds the frame's TLAS and tables, then
/// dispatches rays.
///
/// The class IS the graph's TPass. Acceleration-structure builds no longer
/// live in the graph: the device executes the pending BLAS batch and the TLAS
/// rebuild (carried by RenderResult) at the start of the frame's execution,
/// ahead of every pass. All table buffers are graph-created transients
/// resolved at Compile.
class PathTracingPass final : public IRHIRayTracingPass {
  public:
    static constexpr StringView Name = "PathTracingPass";
    [[nodiscard]] auto GetName() const noexcept -> StringView override { return Name; }

    /// Static pipeline descriptor for PipelineRegistry::Register<PathTracingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> RayTracingPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "RayTracing" / "RayTracing.slang";
        return RayTracingPipelineRequest{
            .RayGeneration = {.SourcePath = ShaderPath, .EntryPoint = "rayGenMain"},
            .MissEntries =
                {
                    {.SourcePath = ShaderPath, .EntryPoint = "missMain"},
                    {.SourcePath = ShaderPath, .EntryPoint = "shadowMissMain"},
                },
            .HitGroups =
                {
                    {.Type       = ShaderRayTracingHitGroupType::Triangles,
                     .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain"}},
                    {.Type       = ShaderRayTracingHitGroupType::Triangles,
                     .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "shadowClosestHitMain"}},
                },
        };
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::Storage, RHIAccess::Write>   Output;
        RGTextureView<RHITextureUsage::Storage, RHIAccess::Write>   PrimaryNormal;
        RGTextureView<RHITextureUsage::Storage, RHIAccess::Write>   PrimaryEntityId;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>      Instances;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>      Geometries;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>      Materials;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>      Lights;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>     Frame;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>     View;
        // ── Passthrough (declare no access) ──
        /// The frame's TLAS, bound for the trace; built by the frame-start
        /// AS phase, not by this pass.
        RHIRef<RHITopLevelAccelerationStructure> TlasRef       = nullptr;
        RHIRef<RHISampler>                       LinearSampler = nullptr;
        PathTracingExtent                      Extent        = {};
    };

    PathTracingPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!GetShaderBindingSet() || !m_Parameter->TlasRef || !m_Parameter->Output.Resolved ||
            !m_Parameter->PrimaryNormal.Resolved || !m_Parameter->PrimaryEntityId.Resolved ||
            !m_Parameter->LinearSampler || !m_Parameter->Instances.Resolved ||
            !m_Parameter->Geometries.Resolved || !m_Parameter->Materials.Resolved ||
            !m_Parameter->Lights.Resolved || !m_Parameter->Frame.Resolved || !m_Parameter->View.Resolved ||
            m_Parameter->Extent.Width == 0 || m_Parameter->Extent.Height == 0)
            return std::unexpected(ErrorMessage("Path tracing pass resources are not ready"));

        // The whole descriptor set must be bound in one batch: the binding
        // set validates that every reflected slot of the set is covered.
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rayTracing.tlas", RHITlasView{.Tlas = m_Parameter->TlasRef}},
                RHIShaderBindingRequest{"g_rayTracing.output", m_Parameter->Output.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.primaryNormal", m_Parameter->PrimaryNormal.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.primaryEntityId", m_Parameter->PrimaryEntityId.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.instances", m_Parameter->Instances.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.geometryTable.records", m_Parameter->Geometries.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.materials", m_Parameter->Materials.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.lights", m_Parameter->Lights.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.frame", m_Parameter->Frame.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.view", m_Parameter->View.ResolvedView()},
                RHIShaderBindingRequest{"g_rayTracing.linearSampler", RHISamplerView{.Sampler = m_Parameter->LinearSampler}},
            }); !R)
            return std::unexpected(R.error());

        TraceRays(m_Parameter->Extent.Width, m_Parameter->Extent.Height);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
