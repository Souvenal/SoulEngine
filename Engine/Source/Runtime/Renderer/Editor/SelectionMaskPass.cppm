module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionMaskPass;

import Core;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

struct SelectionMaskPassInput {
    RHIRef<RHIRenderTarget>                 Mask = nullptr;
    RHIRef<RHITransientConstantBuffer>      FrameBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>      ViewBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> InstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> GeometryBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> IndirectBuffer = nullptr;
    Uint32                                   DrawCount = 0;
};

class SelectionMaskPass final : public IRHIGraphicsPass {
  public:
    explicit SelectionMaskPass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass("SelectionMaskPass", std::move(Pipeline)) {}

    auto SetInput(SelectionMaskPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet() || !Input.Mask || !Input.FrameBuffer || !Input.ViewBuffer ||
            !Input.InstanceBuffer || !Input.GeometryBuffer || !Input.IndirectBuffer || Input.DrawCount == 0)
            return std::unexpected(ErrorMessage("Selection mask pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = Input.Mask,
                 .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(Input.Mask->GetWidth()),
                    static_cast<Float32>(Input.Mask->GetHeight()));
        SetScissorRect(0, 0, Input.Mask->GetWidth(), Input.Mask->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskFrameView.frame", std::move(Input.FrameBuffer), true},
                RHIShaderBindingRequest{"g_selectionMaskFrameView.view", std::move(Input.ViewBuffer), true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskDraw.instances", std::move(Input.InstanceBuffer), true},
                RHIShaderBindingRequest{"g_selectionMaskDraw.geometryTable.records", std::move(Input.GeometryBuffer), true},
            }); !R)
            return std::unexpected(R.error());
        DrawIndirect(std::move(Input.IndirectBuffer), 0, Input.DrawCount);
        return {};
    }

  private:
    SelectionMaskPassInput m_Input = {};
};

} // namespace SoulEngine
