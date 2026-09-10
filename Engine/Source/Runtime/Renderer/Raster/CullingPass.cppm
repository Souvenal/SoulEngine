module;

#include <hlsl++.h>

export module Renderer:RasterPasses.CullingPass;

import Core;
import RHI;
import RenderGraph;
import Scene;

export import std;

export namespace SoulEngine {

struct CullingPushConstants {
    Uint32 InstanceCount = 0;
    Uint32 CommandCount  = 0;
};

/// @brief Per-view culling pass (placeholder: copies instances one-to-one).
///
/// Future: frustum-culls instances into ViewInstanceBuffer via atomic append
/// and regenerates ViewIndirectBuffer grouped by geometryID on the GPU.
class CullingPass final : public IRHIComputePass {
  public:
    static constexpr StringView Name = "CullingPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<CullingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "CullingCompute.slang";
        return ComputePipelineRequest{
            .ComputeEntry = {.SourcePath = ShaderPath, .EntryPoint = "cullMain"},
        };
    }

    struct Parameter {
        RGStorageBufferUAV ViewInstances  = {};
        RGStorageBufferUAV ViewIndirect   = {};
        RGStorageBufferUAV ViewCounter    = {};
        RGStorageBufferSRV SceneInstances = {};
        RGStorageBufferSRV SceneIndirect  = {};
        RGStorageBufferSRV GeometryTable  = {};
        Uint32 InstanceCount = 0;  ///< CPU passthrough: dispatch size.
        Uint32 CommandCount  = 0;
    };

    CullingPass(Parameter In, RHIRef<RHIComputePipeline> Pipeline)
        : IRHIComputePass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!m_Parameter.SceneInstances.Ref || !m_Parameter.SceneIndirect.Ref ||
            !m_Parameter.GeometryTable.Ref || !m_Parameter.ViewInstances.Ref || !m_Parameter.ViewIndirect.Ref ||
            !m_Parameter.ViewCounter.Ref || m_Parameter.InstanceCount == 0)
            return std::unexpected(ErrorMessage("Culling pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_cullPass.sceneInstances", m_Parameter.SceneInstances.Ref, true},
                RHIShaderBindingRequest{"g_cullPass.sceneIndirectCommands", m_Parameter.SceneIndirect.Ref, true},
                RHIShaderBindingRequest{"g_cullPass.geometryTable.records", m_Parameter.GeometryTable.Ref, true},
                RHIShaderBindingRequest{"g_cullPass.viewInstances", m_Parameter.ViewInstances.Ref, false},
                RHIShaderBindingRequest{"g_cullPass.viewIndirectCommands", m_Parameter.ViewIndirect.Ref, false},
                RHIShaderBindingRequest{"g_cullPass.viewCounter", m_Parameter.ViewCounter.Ref, false},
            }); !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0, CullingPushConstants{
                .InstanceCount = m_Parameter.InstanceCount,
                .CommandCount = m_Parameter.CommandCount,
            }); !R)
            return std::unexpected(R.error());

        Dispatch((std::max(m_Parameter.InstanceCount, m_Parameter.CommandCount) + 63) / 64);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
