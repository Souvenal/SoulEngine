module;

export module Vulkan:Pipeline;

import vulkan;

import RHI;
import std;

import :Types;
import :Shader;
import :Descriptor;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// GraphicsPipeline — concrete Vulkan pipeline owned by SPtr<RHI::GraphicsPipeline>
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan graphics pipeline with shared bindless pipeline layout.
/// Created via the static `Create` factory. Callers hold an
/// `SPtr<RHI::GraphicsPipeline>` and pass it to `CommandList::BindPipeline()`.
class GraphicsPipeline final : public RHI::GraphicsPipeline {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    GraphicsPipeline() = default;

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    auto operator=(const GraphicsPipeline&) -> GraphicsPipeline& = delete;

    /// Create a Vulkan graphics pipeline from an RHI descriptor.
    ///
    /// Uses the global bindless pipeline layout shared by all pipelines in
    /// this backend. Shader modules are transient — destroyed when this
    /// function returns.
    [[nodiscard]] static auto Create(vk::raii::Device& Device,
                                      const GraphicsPipelineDesc& Desc,
                                      const DescriptorManager& Manager)
        -> std::expected<SPtr<GraphicsPipeline>, ErrorMessage> {

        // ── Shader stages ──────────────────────────────────────────────
        auto ShaderStates = GraphicsShaderStates::Create(Device, Desc);
        if (!ShaderStates)
            return std::unexpected(ShaderStates.error());
        Uint32 StageCount = static_cast<Uint32>(ShaderStates->StageInfos.size());
        auto   VertexInputCI = ShaderStates->GetPipelineVertexInputStateCI();

        // Most pipeline state is baked at creation time and cannot change,
        // but some can be changed at draw time without recreating the pipeline.
        //
        // e.g. viewport size, line width, blend constants.
        //
        // Declaring these dynamic states will cause the config of these value to be ignored.
        std::array<vk::DynamicState, 2> DynamicStates{
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };
        vk::PipelineDynamicStateCreateInfo DynamicStateCI{
            .dynamicStateCount = static_cast<Uint32>(DynamicStates.size()),
            .pDynamicStates    = DynamicStates.data(),
        };

        // If specifying statically:
        //
        // Viewport describes the region of the framebuffer to render to,
        // and remember the size of the swapchain images may be different from
        // the window size, so we'll stick to the swapchain extent.
        //
        // Almost always (0, 0) -> (width, height)
        // Defines the transformation from image to framebuffer,
        // so only the targeted region of the framebuffer will be affected.
        //
        // minDepth and maxDepth specify the depth range for the framebuffer,
        // must be within [0.0, 1.0], but min can be higher than max.
        // If nothing special, just use standard (0.0, 1.0).
        // vk::Viewport Viewport{
        //     .x = 0.0f, .y = 0.0f,
        //     .width = static_cast<float>(SwapchainExtent.width),
        //     .height = static_cast<float>(SwapchainExtent.height),
        //     .minDepth = 0.0f, .maxDepth = 1.0f,
        // };
        //
        // Scissor defines the region in which pixels will be stored,
        // so outside the rect, no storage.
        // vk::Rect2D Scissor{
        //     .offset = vk::Offset2D{0, 0},
        //     .extent = SwapchainExtent,
        // };
        vk::PipelineViewportStateCreateInfo ViewportStateCI{
            .viewportCount = 1,
            .scissorCount  = 1,
        };

        // TODO: Support tessellation and geometry shader
        vk::PipelineInputAssemblyStateCreateInfo InputAssemblyCI{
            .topology = vk::PrimitiveTopology::eTriangleList,
        };

        // Besides rasterization, rasterizer in vulkan also performs
        // depth testing, face culling and scissor test.
        //
        // Can also configure fill entire polygons or just the edges (wireframe-rendering).
        //
        // TODO: shadow maps
        vk::PipelineRasterizationStateCreateInfo RasterizerCI{
            // If true, fragments beyond the near and far planes are clamped to them
            // instead of discarded. Useful for *shadow mapping*.
            // Requires a GPU feature.
            .depthClampEnable        = false,
            // If true, rasterization is disabled, basically disabling any output.
            .rasterizerDiscardEnable = false,
            // fill / line / point (line and point require GPU feature)
            // TODO: support all of these in RHI
            // .polygonMode = Desc.Rasterizer.FillMode ? vk::PolygonMode::eFill : vk::PolygonMode::eLine,
            .polygonMode             = vk::PolygonMode::eFill,
            // none / back / front
            // TODO: support all of these in RHI
            // .cullMode = Desc.Rasterizer.CullMode ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone,
            .cullMode                = vk::CullModeFlagBits::eNone,
            // clockwise / counter-clockwise
            // TODO: support changing in RHI
            .frontFace               = vk::FrontFace::eCounterClockwise,
            // describe the thickness in terms of number of fragments
            // thicker than 1.0f needs *wideLines* GPU feature
            // .lineWidth = Desc.Rasterizer.LineWidth,
            .lineWidth               = 1.0f};

        // Why is MSAA cheaper than SSAA?
        // Because MSAA is selective, only in edges of polygons,
        // and there are hardware optimizations for MSAA.
        //
        // TODO: implement MSAA
        vk::PipelineMultisampleStateCreateInfo MultisampleCI{
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
        };

        // TODO: configure depth and stencil testing
        // vk::PipelineDepthStencilStateCreateInfo DepthStencilCI{
        //     .depthTestEnable = Desc.DepthStencil.DepthTestEnable,
        //     .depthWriteEnable = Desc.DepthStencil.DepthWriteEnable,
        //     .depthCompareOp = vk::CompareOp::eLessOrEqual,
        // };

        // After fragment shader returns a color, it needs to be combined with
        // that in the framebuffer. There are 2 ways:
        // - mix old and new
        // - combine old and new with bitwise operations
        //
        // PipelineColorBlendAttachmentState is for per attached framebuffer
        // PipelineColorBlendStateCreateInfo is for global settings
        //
        // TODO: expose this in RHI
        // write through
        vk::PipelineColorBlendAttachmentState BlendAttachment{
            // .blendEnable = Desc.Blend.Attachments[0].BlendEnable,
            .blendEnable    = vk::False,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };
        // Alpha blending
        // vk::PipelineColorBlendAttachmentState BlendAttachment{
        //     .blendEnable         = vk::True,
        //     .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        //     .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        //     .colorBlendOp        = vk::BlendOp::eAdd,
        //     .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        //     .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        //     .alphaBlendOp        = vk::BlendOp::eAdd,
        //     .colorWriteMask      = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        //     vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        // };
        vk::PipelineColorBlendStateCreateInfo BlendCI{
            // set to True if we want bitwise op,
            // which will disable blending settings above
            .logicOpEnable   = vk::False,
            .logicOp         = vk::LogicOp::eCopy,
            .attachmentCount = 1,
            .pAttachments    = &BlendAttachment,
        };

        vk::Format ColorVkFormat = ToVkFormat(Desc.ColorFormat);

        // Dynamic rendering allows us to specify color, depth, stencil attachments directly
        // after the pipeline is created
        // TODO: change according to the actual shaders
        vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> PipelineChain = {
            {.stageCount          = StageCount,
             .pStages             = ShaderStates->StageInfos.data(),
             .pVertexInputState   = &VertexInputCI,
             .pInputAssemblyState = &InputAssemblyCI,
             .pViewportState      = &ViewportStateCI,
             .pRasterizationState = &RasterizerCI,
             .pMultisampleState   = &MultisampleCI,
             // .pDepthStencilState = &DepthStencilCI,
             .pColorBlendState    = &BlendCI,
             .pDynamicState       = &DynamicStateCI,
             .layout              = Manager.GetPipelineLayout(),
             // using dynamic rendering rather than traditional render pass
             .renderPass          = nullptr},
            {.colorAttachmentCount = 1, .pColorAttachmentFormats = &ColorVkFormat}};

        auto [PipelineResult, Pipeline] =
            Device.createGraphicsPipeline(nullptr, PipelineChain.get<vk::GraphicsPipelineCreateInfo>());
        if (PipelineResult != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Core::Format("Failed to create graphics pipeline: {}", vk::to_string(PipelineResult))));

        auto Ret = std::make_shared<GraphicsPipeline>();
        Ret->m_Pipeline = std::move(Pipeline);
        return Ret;
    }

    /// Return the native VkPipeline handle.
    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *m_Pipeline;
    }

  private:
    vk::raii::Pipeline m_Pipeline = nullptr;
};

} // namespace SoulEngine::RHI::Vulkan
