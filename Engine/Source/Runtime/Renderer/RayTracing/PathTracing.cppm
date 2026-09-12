export module Renderer:RayTracing.PathTracing;

import Core;
import RHI;
import RenderGraph;

export import std;

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

    /// Static pipeline descriptor for PipelineRegistry::Register<PathTracingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> RayTracingPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "RayTracing" / "RayTracing.slang";
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
        RGStorageTextureUAV Output          = {};
        RGStorageTextureUAV PrimaryNormal   = {};
        RGStorageTextureUAV PrimaryEntityId = {};
        RGStorageBufferSRV  Instances       = {};
        RGStorageBufferSRV  Geometries      = {};
        RGStorageBufferSRV  Materials       = {};
        RGStorageBufferSRV  Lights          = {};
        RGConstantBufferSRV Frame           = {};
        RGConstantBufferSRV View            = {};
        // ── Passthrough (declare no access) ──
        /// The frame's TLAS, bound for the trace; built by the frame-start
        /// AS phase, not by this pass.
        RHIRef<RHITopLevelAccelerationStructure> TlasRef       = nullptr;
        RHIRef<RHISampler>                       LinearSampler = nullptr;
        RHIRefArray<RHISampledTexture>           Textures      = {};
        PathTracingExtent                      Extent        = {};
    };

    PathTracingPass(Parameter In, RHIRef<RHIRayTracingPipeline> Pipeline)
        : IRHIRayTracingPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!GetShaderBindingSet() || !m_Parameter.TlasRef || !m_Parameter.Output.Ref ||
            !m_Parameter.PrimaryNormal.Ref || !m_Parameter.PrimaryEntityId.Ref ||
            !m_Parameter.LinearSampler || !m_Parameter.Textures || !m_Parameter.Instances.Ref ||
            !m_Parameter.Geometries.Ref || !m_Parameter.Materials.Ref || !m_Parameter.Lights.Ref ||
            !m_Parameter.Frame.Ref || !m_Parameter.View.Ref ||
            m_Parameter.Extent.Width == 0 || m_Parameter.Extent.Height == 0)
            return std::unexpected(ErrorMessage("Path tracing pass resources are not ready"));

        // The whole descriptor set must be bound in one batch: the binding
        // set validates that every reflected slot of the set is covered.
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rayTracing.tlas", m_Parameter.TlasRef, true},
                RHIShaderBindingRequest{"g_rayTracing.output", m_Parameter.Output.Ref, false},
                RHIShaderBindingRequest{"g_rayTracing.primaryNormal", m_Parameter.PrimaryNormal.Ref, false},
                RHIShaderBindingRequest{"g_rayTracing.primaryEntityId", m_Parameter.PrimaryEntityId.Ref, false},
                RHIShaderBindingRequest{"g_rayTracing.instances", m_Parameter.Instances.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.geometryTable.records", m_Parameter.Geometries.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.materials", m_Parameter.Materials.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.lights", m_Parameter.Lights.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.frame", m_Parameter.Frame.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.view", m_Parameter.View.Ref, true},
                RHIShaderBindingRequest{"g_rayTracing.linearSampler", m_Parameter.LinearSampler, true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindBindlessResource(std::move(m_Parameter.Textures)); !R)
            return std::unexpected(R.error());

        TraceRays(m_Parameter.Extent.Width, m_Parameter.Extent.Height);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
