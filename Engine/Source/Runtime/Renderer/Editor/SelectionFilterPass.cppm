module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionFilterPass;

import Core;
import RHI;
import RenderGraph;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Filters culling output instances by entity ID for editor selection.
class SelectionFilterPass final : public IRHIComputePass {
  public:
    static constexpr StringView Name = "SelectionFilterPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionFilterPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionFilter.slang";
        return ComputePipelineRequest{
            .ComputeEntry = {.SourcePath = ShaderPath, .EntryPoint = "filterMain"},
        };
    }

    struct Parameter {
        RGStorageBufferSRV ViewInstances     = {};
        RGStorageBufferSRV ViewIndirect      = {};
        RGStorageBufferUAV SelectedInstances = {};
        RGStorageBufferUAV SelectedIndirect  = {};
        /// Created with zero-initialized InitialData (historical crash fix):
        /// the filter accumulates per-command counts via InterlockedAdd.
        RGStorageBufferUAV SelectedCounters  = {};
        Uint32 CommandCount     = 0;
        Uint32 SelectedEntityId = 0;
    };

    SelectionFilterPass(Parameter In, RHIRef<RHIComputePipeline> Pipeline)
        : IRHIComputePass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!m_Parameter.ViewInstances.Ref || !m_Parameter.ViewIndirect.Ref ||
            !m_Parameter.SelectedInstances.Ref || !m_Parameter.SelectedIndirect.Ref ||
            !m_Parameter.SelectedCounters.Ref || m_Parameter.CommandCount == 0)
            return std::unexpected(ErrorMessage("Selection filter pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionFilter.sceneInstances", m_Parameter.ViewInstances.Ref, true},
                RHIShaderBindingRequest{"g_selectionFilter.sceneIndirectCommands", m_Parameter.ViewIndirect.Ref, true},
                RHIShaderBindingRequest{"g_selectionFilter.selectedInstances", m_Parameter.SelectedInstances.Ref, false},
                RHIShaderBindingRequest{"g_selectionFilter.selectedIndirectCommands", m_Parameter.SelectedIndirect.Ref, false},
                RHIShaderBindingRequest{"g_selectionFilter.commandCounters", m_Parameter.SelectedCounters.Ref, false},
            }); !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0, m_Parameter.SelectedEntityId); !R)
            return std::unexpected(R.error());

        Dispatch(m_Parameter.CommandCount);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
