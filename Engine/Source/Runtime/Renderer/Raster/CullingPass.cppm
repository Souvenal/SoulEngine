module;

#include <hlsl++.h>

export module Renderer:Raster.CullingPass;

import Core;
import RHI;
import RenderGraph;
import Scene;

import std;
import ShaderCompiler;

namespace SoulEngine {

export struct CullingPushConstants {
    Uint32 InstanceCount = 0;
    Uint32 CommandCount  = 0;
};

/// @brief Per-view culling pass (placeholder: copies instances one-to-one).
///
/// Future: frustum-culls instances into ViewInstanceBuffer via atomic append and regenerates
/// ViewIndirectBuffer grouped by geometryID on the GPU.
export class CullingPass final : public IRHIComputePass {
  public:
    static constexpr StringView Name = "CullingPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<CullingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Raster" / "CullingCompute.slang";
        return ComputePipelineRequest{
            .ComputeEntry = {.SourcePath = ShaderPath, .EntryPoint = "cullMain"},
        };
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> ViewInstances;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> ViewIndirect;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> ViewCounter;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>  SceneInstances;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>  SceneIndirect;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>  GeometryTable;
        Uint32                                                  InstanceCount = 0; ///< CPU passthrough: dispatch size.
        Uint32                                                  CommandCount  = 0;
    };

    CullingPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!m_Parameter->SceneInstances.Resolved || !m_Parameter->SceneIndirect.Resolved ||
            !m_Parameter->GeometryTable.Resolved || !m_Parameter->ViewInstances.Resolved ||
            !m_Parameter->ViewIndirect.Resolved || !m_Parameter->ViewCounter.Resolved ||
            m_Parameter->InstanceCount == 0)
            return std::unexpected(ErrorMessage("Culling pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_cullPass.sceneInstances", m_Parameter->SceneInstances.ResolvedView()},
                RHIShaderBindingRequest{"g_cullPass.sceneIndirectCommands", m_Parameter->SceneIndirect.ResolvedView()},
                RHIShaderBindingRequest{"g_cullPass.geometryTable.records", m_Parameter->GeometryTable.ResolvedView()},
                RHIShaderBindingRequest{"g_cullPass.viewInstances", m_Parameter->ViewInstances.ResolvedView()},
                RHIShaderBindingRequest{"g_cullPass.viewIndirectCommands", m_Parameter->ViewIndirect.ResolvedView()},
                RHIShaderBindingRequest{"g_cullPass.viewCounter", m_Parameter->ViewCounter.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0,
                                   CullingPushConstants{
                                       .InstanceCount = m_Parameter->InstanceCount,
                                       .CommandCount  = m_Parameter->CommandCount,
                                   });
            !R)
            return std::unexpected(R.error());

        Dispatch((std::max(m_Parameter->InstanceCount, m_Parameter->CommandCount) + 63) / 64);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
