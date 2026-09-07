module;

#include <hlsl++.h>

export module Renderer:RasterPasses.CullingPass;

import Core;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

struct CullingPassInput {
    RHIRef<RHITransientShaderStorageBuffer> SceneInstanceBuffer  = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> SceneIndirectBuffer  = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> GeometryBuffer       = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> ViewInstanceBuffer   = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> ViewIndirectBuffer   = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> ViewCounterBuffer    = nullptr;
    Uint32                                  InstanceCount        = 0;
    Uint32                                  CommandCount         = 0;
};

struct CullingPushConstants {
    Uint32 InstanceCount = 0;
    Uint32 CommandCount = 0;
};

/// @brief Per-view culling pass (placeholder: copies instances one-to-one).
///
/// Future: frustum-culls instances into ViewInstanceBuffer via atomic append
/// and regenerates ViewIndirectBuffer grouped by geometryID on the GPU.
class CullingPass final : public IRHIComputePass {
  public:
    explicit CullingPass(RHIRef<RHIComputePipeline> Pipeline)
        : IRHIComputePass("CullingPass", std::move(Pipeline)) {}

    auto SetInput(CullingPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet() || !Input.SceneInstanceBuffer || !Input.SceneIndirectBuffer ||
            !Input.GeometryBuffer || !Input.ViewInstanceBuffer || !Input.ViewIndirectBuffer ||
            !Input.ViewCounterBuffer || Input.InstanceCount == 0)
            return std::unexpected(ErrorMessage("Culling pass resources are not ready"));

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_cullPass.sceneInstances", std::move(Input.SceneInstanceBuffer), true},
                RHIShaderBindingRequest{"g_cullPass.sceneIndirectCommands", std::move(Input.SceneIndirectBuffer), true},
                RHIShaderBindingRequest{"g_cullPass.geometryTable.records", std::move(Input.GeometryBuffer), true},
                RHIShaderBindingRequest{"g_cullPass.viewInstances", std::move(Input.ViewInstanceBuffer), false},
                RHIShaderBindingRequest{"g_cullPass.viewIndirectCommands", std::move(Input.ViewIndirectBuffer), false},
                RHIShaderBindingRequest{"g_cullPass.viewCounter", std::move(Input.ViewCounterBuffer), false},
            }); !R)
            return std::unexpected(R.error());

        if (auto R = PushConstants(0, CullingPushConstants{
                .InstanceCount = Input.InstanceCount,
                .CommandCount = Input.CommandCount,
            }); !R)
            return std::unexpected(R.error());

        Dispatch((std::max(Input.InstanceCount, Input.CommandCount) + 63) / 64);
        return {};
    }

  private:
    CullingPassInput m_Input = {};
};

} // namespace SoulEngine
