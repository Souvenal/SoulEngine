module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionFilterPass;

import Core;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

struct SelectionFilterPassInput {
    RHIRef<RHITransientShaderStorageBuffer> SceneInstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> SceneIndirectBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> SelectedInstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> SelectedIndirectBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> CommandCounterBuffer = nullptr;
    Uint32 CommandCount = 0;
    Uint32 SelectedEntityId = GBuffer::BackgroundEntityId;
};

/// @brief Filters culling output instances by entity ID for editor selection.
class SelectionFilterPass final : public IRHIComputePass {
  public:
    explicit SelectionFilterPass(RHIRef<RHIComputePipeline> Pipeline)
        : IRHIComputePass("SelectionFilterPass", std::move(Pipeline)) {}

    auto SetInput(SelectionFilterPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet() || !Input.SceneInstanceBuffer || !Input.SceneIndirectBuffer ||
            !Input.SelectedInstanceBuffer || !Input.SelectedIndirectBuffer || !Input.CommandCounterBuffer ||
            Input.CommandCount == 0)
            return std::unexpected(ErrorMessage("Selection filter pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionFilter.sceneInstances", std::move(Input.SceneInstanceBuffer), true},
                RHIShaderBindingRequest{"g_selectionFilter.sceneIndirectCommands", std::move(Input.SceneIndirectBuffer), true},
                RHIShaderBindingRequest{"g_selectionFilter.selectedInstances", std::move(Input.SelectedInstanceBuffer), false},
                RHIShaderBindingRequest{"g_selectionFilter.selectedIndirectCommands", std::move(Input.SelectedIndirectBuffer), false},
                RHIShaderBindingRequest{"g_selectionFilter.commandCounters", std::move(Input.CommandCounterBuffer), false},
            }); !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0, Input.SelectedEntityId); !R)
            return std::unexpected(R.error());

        Dispatch(Input.CommandCount);
        return {};
    }

  private:
    SelectionFilterPassInput m_Input = {};
};

} // namespace SoulEngine
