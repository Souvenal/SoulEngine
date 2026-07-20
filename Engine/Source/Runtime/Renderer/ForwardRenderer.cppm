module;

// needed for offsetof
#include <cstddef>
#include <hlsl++.h>

export module Renderer:ForwardRenderer;

import Core;
import RHI;
import Resource;
import Scene;

import :IRenderer;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) ForwardFrameConstants {
    Float32                         Time = 0.0f;
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity =
        hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor =
        hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
};
static_assert(sizeof(ForwardFrameConstants) == 48, "ForwardFrameConstants must match ForwardPbr.slang FrameData std140 layout");
static_assert(offsetof(ForwardFrameConstants, Time) == 0, "ForwardFrameConstants::Time must match FrameData.time");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightDirectionIntensity) == 16,
              "ForwardFrameConstants::DirectionalLightDirectionIntensity must match FrameData");
static_assert(offsetof(ForwardFrameConstants, DirectionalLightColor) == 32,
              "ForwardFrameConstants::DirectionalLightColor must match FrameData");

/// @brief Constant buffer layout matching ForwardPbr.slang ViewData.
struct alignas(16) ForwardViewConstants {
    alignas(16) hlslpp::float4x4         ViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
};
static_assert(sizeof(ForwardViewConstants) == 80, "ForwardViewConstants must match ForwardPbr.slang ViewData std140 layout");

/// @brief Constant buffer layout matching ForwardPbr.slang MaterialData.
struct alignas(16) ForwardMaterialConstants {
    alignas(16) hlslpp::interop::float4 BaseColorFactor =
        hlslpp::interop::float4{hlslpp::float4{0.62f, 0.28f, 0.10f, 1.0f}};
    Float32 MetallicFactor  = 0.0f;
    Float32 RoughnessFactor = 0.42f;
};
static_assert(sizeof(ForwardMaterialConstants) == 32,
              "ForwardMaterialConstants must match ForwardPbr.slang MaterialData std140 layout");
static_assert(offsetof(ForwardMaterialConstants, BaseColorFactor) == 0,
              "ForwardMaterialConstants::BaseColorFactor must match MaterialData.baseColorFactor");
static_assert(offsetof(ForwardMaterialConstants, MetallicFactor) == 16,
              "ForwardMaterialConstants::MetallicFactor must match MaterialData.metallicFactor");
static_assert(offsetof(ForwardMaterialConstants, RoughnessFactor) == 20,
              "ForwardMaterialConstants::RoughnessFactor must match MaterialData.roughnessFactor");

struct ForwardViewParameterState {
    String                ViewConstantBufferKey = {};
    RHI::ShaderParameters Parameters            = {};
};

/// @brief Single-material metallic-roughness forward renderer.
class ForwardRenderer final : public IRenderer {
  public:
    ForwardRenderer() = default;
    ~ForwardRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().CurrentApplicationDir() / "Shaders" / "ForwardPbr.slang";

        m_Pipeline = Resource::Manager::Get().RequestGraphicsPipelineRef(Resource::GraphicsPipelineRequest{
            .VertEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "vertMain",
            },
            .FragEntry = {
                .SourcePath = ShaderPath,
                .EntryPoint = "fragMain",
            },
            .VertexInputLayout = MakeVertexInputLayout(),
            .DepthFormat       = RHI::Format::D32_SFLOAT,
        });
        if (!m_Pipeline)
            return std::unexpected(ErrorMessage("Forward PBR graphics pipeline request failed"));

        auto& Resources = Resource::Manager::Get();
        m_FrameConstants = Resources.RequestConstantBufferRef(
            "forward_pbr_frame_constants", {.Size = sizeof(ForwardFrameConstants)});
        if (!m_FrameConstants)
            return std::unexpected(ErrorMessage("Forward PBR frame constant buffer request failed"));

        m_ViewConstants =
            Resources.RequestConstantBufferRef("forward_pbr_view_constants", {.Size = sizeof(ForwardViewConstants)});
        if (!m_ViewConstants)
            return std::unexpected(ErrorMessage("Forward PBR view constant buffer request failed"));

        m_MaterialConstants = Resources.RequestConstantBufferRef(
            "forward_pbr_material_constants", {.Size = sizeof(ForwardMaterialConstants)});
        if (!m_MaterialConstants)
            return std::unexpected(ErrorMessage("Forward PBR material constant buffer request failed"));

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline                 = {};
        m_FrameConstants           = {};
        m_ViewConstants            = {};
        m_MaterialConstants        = {};
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto* FrameCB = Resource::Manager::Get().TryGetReady(m_FrameConstants);
        if (!FrameCB)
            return Result;

        const auto FrameData = BuildFrameConstants(Scene.Time);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, Scene, View, FrameCB, FrameData); !R)
                return std::unexpected(R.error().Append("Forward PBR view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] static auto MakeVertexInputLayout() -> RHI::VertexInputLayoutDesc {
        return RHI::VertexInputLayoutDesc{
            .Bindings = {
                {.Binding = 0, .Stride = sizeof(hlslpp::interop::float3)},
                {.Binding = 1, .Stride = sizeof(hlslpp::interop::float3)},
            },
            .Attributes = {
                {.Location = 0, .Binding = 0, .Format = RHI::Format::R32G32B32_SFLOAT, .Offset = 0},
                {.Location = 1, .Binding = 1, .Format = RHI::Format::R32G32B32_SFLOAT, .Offset = 0},
            },
        };
    }

    [[nodiscard]] auto RenderView(RHI::CommandList&                 CmdList,
                                  const Scene::SceneSnapshot&     Scene,
                                  const Scene::RenderViewSnapshot& View,
                                  RHI::ConstantBuffer*             FrameCB,
                                  const ForwardFrameConstants&     FrameData)
        -> std::expected<void, ErrorMessage> {
        auto& Resources = Resource::Manager::Get();

        auto* ColorRT = Resources.TryGetReady(View.ColorRT);
        auto* DepthRT = Resources.TryGetReady(View.DepthRT);
        auto* ViewCB  = Resources.TryGetReady(m_ViewConstants);
        auto* Pipeline = Resources.TryGetReady(m_Pipeline);
        auto* MaterialCB = Resources.TryGetReady(m_MaterialConstants);
        if (!ColorRT || !DepthRT || !ViewCB || !Pipeline || !MaterialCB) {
            return {};
        }

        auto& Parameters = GetViewParameters(View, *Pipeline);
        if (auto R = Parameters.SetConstantBuffer("g_forwardFrameView.frame", FrameCB, &FrameData, sizeof(FrameData)); !R)
            return std::unexpected(R.error().Append("Forward PBR frame parameter binding failed"));

        const auto ViewData = BuildViewConstants(View);
        if (auto R = Parameters.SetConstantBuffer("g_forwardFrameView.view", ViewCB, &ViewData, sizeof(ViewData)); !R)
            return std::unexpected(R.error().Append("Forward PBR view parameter binding failed"));

        const auto MaterialData = BuildMaterialConstants();
        if (auto R = Parameters.SetConstantBuffer(
                "g_forwardMaterial.material", MaterialCB, &MaterialData, sizeof(MaterialData));
            !R) {
            return std::unexpected(R.error().Append("Forward PBR material parameter binding failed"));
        }
        RHI::Pass Pass{
            .Desc = RHI::RenderingDesc{
                .ColorAttachment = {
                    .TexturePtr = ColorRT,
                    .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f},
                },
                .DepthAttachment = RHI::DepthAttachmentDesc{
                    .TexturePtr = DepthRT,
                    .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                },
            },
        };
        if (!CmdList.PresentSource)
            CmdList.PresentSource = ColorRT;

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(Pipeline);
        Pass.BindShaderParameters(Pipeline, Parameters);
        for (const auto& Packet : Scene.DrawPackets) {
            auto* PositionVB = Resources.TryGetReady(Packet.PositionVB);
            auto* NormalVB   = Resources.TryGetReady(Packet.NormalVB);
            auto* IB = Resources.TryGetReady(Packet.IB);
            if (!PositionVB || !NormalVB || !IB)
                continue;

            Pass.DrawIndexed(Pipeline,
                             std::array<RHI::VertexBuffer*, RHI::kMaxVertexBufferBindings>{PositionVB, NormalVB},
                             IB);
        }

        CmdList.Passes.push_back(std::move(Pass));
        return {};
    }

    [[nodiscard]] static auto BuildFrameConstants(float Time) -> ForwardFrameConstants {
        return ForwardFrameConstants{
            .Time = Time,
            .DirectionalLightDirectionIntensity =
                hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}},
            .DirectionalLightColor = hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}},
        };
    }

    [[nodiscard]] static auto BuildMaterialConstants() -> ForwardMaterialConstants {
        return {};
    }

    [[nodiscard]] static auto BuildViewConstants(const Scene::RenderViewSnapshot& View) -> ForwardViewConstants {
        return ForwardViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{
                hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const Scene::RenderViewSnapshot& View, const RHI::GraphicsPipeline& Pipeline)
        -> RHI::ShaderParameters& {
        const auto& ViewConstantBufferKey = View.ViewCB.GetKey();
        for (auto& State : m_ViewParameters) {
            if (State.ViewConstantBufferKey != ViewConstantBufferKey)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHI::ShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(ForwardViewParameterState{
            .ViewConstantBufferKey = ViewConstantBufferKey,
            .Parameters            = RHI::ShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    Resource::ResourceRef<RHI::GraphicsPipeline> m_Pipeline                 = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_FrameConstants           = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_ViewConstants            = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_MaterialConstants        = {};
    std::vector<ForwardViewParameterState>               m_ViewParameters          = {};
};

} // namespace SoulEngine::Renderer
