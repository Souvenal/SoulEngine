module;

export module Vulkan:Pipeline;

import vulkan;

import RHI;
import Shader;
import std;

import :Types;
import :Shader;
import :ShaderBindingSet;
import :Context;
import :Debug;

namespace SoulEngine {

[[nodiscard]] auto CountDynamicOffsets(const ShaderReflection& Reflection) -> Uint32 {
    Uint32 Count = 0;
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Type == ShaderResourceType::ConstantBuffer)
            ++Count;
    }
    return Count;
}

struct VulkanReflectedDescriptorBinding {
    String             ParameterPath      = {};
    Uint32             Set                = 0;
    Uint32             Binding            = 0;
    ShaderResourceType Type               = ShaderResourceType::Unknown;
    Uint32             ArrayCount         = 1;
    Uint32             DynamicOffsetIndex = std::numeric_limits<Uint32>::max();
};

[[nodiscard]] auto BuildReflectedBindings(const ShaderReflection& Reflection)
    -> std::vector<VulkanReflectedDescriptorBinding> {
    std::vector<ShaderBinding> Sorted = Reflection.Bindings;
    std::ranges::sort(Sorted, [](const ShaderBinding& Lhs, const ShaderBinding& Rhs) {
        if (Lhs.Set != Rhs.Set)
            return Lhs.Set < Rhs.Set;
        return Lhs.BindingIndex < Rhs.BindingIndex;
    });

    std::vector<VulkanReflectedDescriptorBinding> Result;
    Result.reserve(Sorted.size());
    Uint32 DynamicOffsetIndex = 0;
    for (const auto& Binding : Sorted) {
        auto& Out         = Result.emplace_back();
        Out.ParameterPath = Binding.ParameterPath;
        Out.Set           = Binding.Set;
        Out.Binding       = Binding.BindingIndex;
        Out.Type          = Binding.Type;
        Out.ArrayCount    = Binding.ArrayCount;
        if (Binding.Type == ShaderResourceType::ConstantBuffer)
            Out.DynamicOffsetIndex = DynamicOffsetIndex++;
    }
    return Result;
}

// ═════════════════════════════════════════════════════════════════════════════
// VulkanGraphicsPipeline — concrete Vulkan pipeline owned by ResourceManager.
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan graphics pipeline using the layout owned by its shader binding set.
/// Created via the static `Create` factory. RHICommand lists only observe it.
class VulkanGraphicsPipeline final : public RHIGraphicsPipeline {
  public:
    explicit VulkanGraphicsPipeline(String Name, const RHIGraphicsPipelineDesc& Desc)
        : RHIGraphicsPipeline(std::move(Name), Desc.BindingSet) {}

    ~VulkanGraphicsPipeline() override = default;

    VulkanGraphicsPipeline(const VulkanGraphicsPipeline&)                    = delete;
    auto operator=(const VulkanGraphicsPipeline&) -> VulkanGraphicsPipeline& = delete;

    /// Create a Vulkan graphics pipeline from an RHI descriptor.
    ///
    /// Shader modules are transient — destroyed when this function returns.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     StringView                   Name,
                                     const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<UPtr<VulkanGraphicsPipeline>, ErrorMessage> {

        // Descriptor set layouts always come from the shader binding set, which
        // ResourcePipeline creates before the pipeline.
        auto* BindingSet = Desc.BindingSet.TryGet();
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Graphics pipeline requires a ready shader binding set"));
        auto* VulkanBindingSet = static_cast<VulkanShaderBindingSet*>(BindingSet);
        // ── Shader stages ──────────────────────────────────────────────
        auto ShaderStates = VulkanGraphicsShaderStates::Create(Context.GetDevice(), Context.GetDebugUtils(), Desc);
        if (!ShaderStates)
            return std::unexpected(ShaderStates.error());
        Uint32 StageCount    = static_cast<Uint32>(ShaderStates->StageInfos.size());
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

        bool                                    HasDepth = Desc.DepthFormat != RHIFormat::Unknown;
        vk::PipelineDepthStencilStateCreateInfo DepthStencilCI{
            .depthTestEnable       = Desc.DepthStencil.DepthTestEnable ? vk::True : vk::False,
            .depthWriteEnable      = Desc.DepthStencil.DepthWriteEnable ? vk::True : vk::False,
            .depthCompareOp        = vk::CompareOp::eLessOrEqual,
            .depthBoundsTestEnable = vk::False,
            .stencilTestEnable     = vk::False,
            .front                 = {},
            .back                  = {},
            .minDepthBounds        = 0.0f,
            .maxDepthBounds        = 1.0f,
        };

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
        std::vector<vk::PipelineColorBlendAttachmentState> RHIBlendAttachments(
            static_cast<Uint32>(Desc.ColorFormats.size()),
            vk::PipelineColorBlendAttachmentState{
                // .blendEnable = Desc.Blend.Attachments[0].BlendEnable,
                .blendEnable    = vk::False,
                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
            });
        // Alpha blending
        // vk::PipelineColorBlendAttachmentState RHIBlendAttachment{
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
            .attachmentCount = static_cast<Uint32>(RHIBlendAttachments.size()),
            .pAttachments    = RHIBlendAttachments.data(),
        };

        const auto ColorFormats =
            Desc.ColorFormats | std::views::transform(ToVkFormat) | std::ranges::to<std::vector<vk::Format>>();
        vk::Format DepthVkFormat = HasDepth ? ToVkFormat(Desc.DepthFormat) : vk::Format::eUndefined;

        // Dynamic rendering allows us to specify color, depth, stencil attachments directly
        // after the pipeline is created
        vk::PipelineRenderingCreateInfo RenderingCI{
            .colorAttachmentCount    = static_cast<Uint32>(ColorFormats.size()),
            .pColorAttachmentFormats = ColorFormats.data(),
            .depthAttachmentFormat   = DepthVkFormat,
        };
        vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> PipelineChain = {
            {.stageCount          = StageCount,
             .pStages             = ShaderStates->StageInfos.data(),
             .pVertexInputState   = &VertexInputCI,
             .pInputAssemblyState = &InputAssemblyCI,
             .pViewportState      = &ViewportStateCI,
             .pRasterizationState = &RasterizerCI,
             .pMultisampleState   = &MultisampleCI,
             .pDepthStencilState  = HasDepth ? &DepthStencilCI : nullptr,
             .pColorBlendState    = &BlendCI,
             .pDynamicState       = &DynamicStateCI,
             .layout              = VulkanBindingSet->GetPipelineLayout(),
             // using dynamic rendering rather than traditional render pass
             .renderPass          = nullptr},
            RenderingCI};

        auto [PipelineResult, RHIPipeline] =
            Context.GetDevice().createGraphicsPipeline(nullptr, PipelineChain.get<vk::GraphicsPipelineCreateInfo>());
        if (PipelineResult != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("Failed to create graphics pipeline: {}", vk::to_string(PipelineResult))));

        Context.GetDebugUtils().SetObjectName(*RHIPipeline, Name);

        auto Ret        = std::make_unique<VulkanGraphicsPipeline>(String(Name), Desc);
        Ret->m_Pipeline = std::make_shared<vk::raii::Pipeline>(std::move(RHIPipeline));
        Ret->m_DynamicOffsetCount = CountDynamicOffsets(Desc.Program.Reflection);
        Ret->m_Bindings           = BuildReflectedBindings(Desc.Program.Reflection);
        Ret->m_ColorFormats       = Desc.ColorFormats;
        Ret->m_DepthFormat        = Desc.DepthFormat;
        return Ret;
    }

    /// Return the native VkPipeline handle.
    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_Pipeline);
    }

    [[nodiscard]] auto GetBindings() const -> std::span<const VulkanReflectedDescriptorBinding> {
        return m_Bindings;
    }

    [[nodiscard]] auto FindBinding(StringView ParameterPath, ShaderResourceType Type) const
        -> const VulkanReflectedDescriptorBinding* {
        for (const auto& Binding : m_Bindings) {
            if (Binding.Type == Type && Binding.ParameterPath == ParameterPath)
                return &Binding;
        }
        return nullptr;
    }

    [[nodiscard]] auto GetDynamicOffsetCount() const -> Uint32 {
        return m_DynamicOffsetCount;
    }

    [[nodiscard]] auto IsCompatibleWith(std::span<const RHIFormat> ColorFormats, RHIFormat DepthFormat) const -> bool {
        return std::ranges::equal(m_ColorFormats, ColorFormats) && m_DepthFormat == DepthFormat;
    }

  private:
    SPtr<vk::raii::Pipeline>                         m_Pipeline       = nullptr;
    std::vector<VulkanReflectedDescriptorBinding>    m_Bindings       = {};
    Uint32                                    m_DynamicOffsetCount = 0;
    std::vector<RHIFormat>                    m_ColorFormats       = {};
    RHIFormat                                 m_DepthFormat        = RHIFormat::Unknown;
};

} // namespace SoulEngine
