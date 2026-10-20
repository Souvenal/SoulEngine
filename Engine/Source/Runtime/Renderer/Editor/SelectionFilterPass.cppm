module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionFilterPass;

import Core;
import RHI;
import RenderGraph;
import Scene;

import std;
import ShaderCompiler;

namespace SoulEngine {

/// @brief Filters culling output instances by entity ID for editor selection.
export class SelectionFilterPass final : public IRHIComputePass {
  public:
    static constexpr StringView Name = "SelectionFilterPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionFilterPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Editor" / "SelectionFilter.slang";
        return ComputePipelineRequest{
            .ComputeEntry = {.SourcePath = ShaderPath, .EntryPoint = "filterMain"},
        };
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>                    ViewInstances;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>                    ViewIndirect;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write>                   SelectedInstances;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write>                   SelectedIndirect;
        /// @brief Per-command selected counts.
        ///
        /// Created with zero-initialized InitialData (historical crash fix): the filter accumulates
        /// per-command counts via InterlockedAdd — an RMW, hence the declared Read|Write.
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read | RHIAccess::Write> SelectedCounters;
        Uint32                                                                    CommandCount     = 0;
        Uint32                                                                    SelectedEntityId = 0;
    };

    SelectionFilterPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();

        if (!m_Parameter->ViewInstances.Resolved || !m_Parameter->ViewIndirect.Resolved ||
            !m_Parameter->SelectedInstances.Resolved || !m_Parameter->SelectedIndirect.Resolved ||
            !m_Parameter->SelectedCounters.Resolved || m_Parameter->CommandCount == 0)
            return std::unexpected(ErrorMessage("Selection filter pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionFilter.sceneInstances", m_Parameter->ViewInstances.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionFilter.sceneIndirectCommands",
                                        m_Parameter->ViewIndirect.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionFilter.selectedInstances",
                                        m_Parameter->SelectedInstances.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionFilter.selectedIndirectCommands",
                                        m_Parameter->SelectedIndirect.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionFilter.commandCounters",
                                        m_Parameter->SelectedCounters.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0, m_Parameter->SelectedEntityId); !R)
            return std::unexpected(R.error());

        Dispatch(m_Parameter->CommandCount);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
