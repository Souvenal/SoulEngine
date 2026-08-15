module;

export module Vulkan:Pipeline;

import vulkan;

import RHI;
import Shader;
import std;

import :Types;
import :Shader;
import :Capability;
import :Descriptor;
import :Context;

namespace SoulEngine {

[[nodiscard]] auto ToVkDescriptorType(const ShaderBinding& Binding)
    -> std::expected<vk::DescriptorType, ErrorMessage> {
    switch (Binding.Type) {
    case ShaderResourceType::ConstantBuffer:
        return vk::DescriptorType::eUniformBufferDynamic;
    case ShaderResourceType::StorageBuffer:
        return vk::DescriptorType::eStorageBuffer;
    case ShaderResourceType::SampledTexture:
        return vk::DescriptorType::eSampledImage;
    case ShaderResourceType::StorageTexture:
        return vk::DescriptorType::eStorageImage;
    case ShaderResourceType::Sampler:
        return vk::DescriptorType::eSampler;
    case ShaderResourceType::AccelerationStructure:
        return vk::DescriptorType::eAccelerationStructureKHR;
    case ShaderResourceType::Unknown:
        break;
    }
    return std::unexpected(
        ErrorMessage(Format("Unsupported reflected resource type for '{}'", Binding.ParameterPath)));
}

[[nodiscard]] auto GetMaxRuntimeSampledTextureCount() -> Uint32 {
    const auto& DescriptorIndexingProperties =
        VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    return std::min(DescriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                    DescriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages);
}
[[nodiscard]] auto CreateDescriptorSetLayout(vk::raii::Device&                Device,
                                             std::span<const ShaderBinding> Bindings,
                                             vk::ShaderStageFlags              ShaderStages)
    -> std::expected<vk::raii::DescriptorSetLayout, ErrorMessage> {
    // RHIPipeline layouts are generated from linked shader reflection. The reflected
    // set/binding numbers are consumed only inside Vulkan; renderer code binds by
    // shader parameter path.
    std::vector<vk::DescriptorSetLayoutBinding> VkBindings;
    std::vector<vk::DescriptorBindingFlags>     BindingFlags;
    VkBindings.reserve(Bindings.size());
    BindingFlags.reserve(Bindings.size());

    bool bHasBindingFlags = false;
    for (Uint32 BindingIndex = 0; BindingIndex < Bindings.size(); ++BindingIndex) {
        const auto& Binding = Bindings[BindingIndex];
        auto DescriptorType = ToVkDescriptorType(Binding);
        if (!DescriptorType)
            return std::unexpected(DescriptorType.error());

        const bool   bUnboundedArray = Binding.ArrayCount == std::numeric_limits<Uint32>::max();
        if (bUnboundedArray &&
            (Binding.Type != ShaderResourceType::SampledTexture || BindingIndex + 1 != Bindings.size())) {
            return std::unexpected(ErrorMessage(Format(
                "Reflected runtime array '{}' must be the final sampled-texture binding in its descriptor set",
                Binding.ParameterPath)));
        }
        const Uint32 DescriptorCount = bUnboundedArray ? GetMaxRuntimeSampledTextureCount() : Binding.ArrayCount;
        if (DescriptorCount == 0)
            return std::unexpected(
                ErrorMessage(Format("Reflected binding '{}' has zero descriptor count", Binding.ParameterPath)));

        VkBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding            = Binding.BindingIndex,
            .descriptorType     = *DescriptorType,
            .descriptorCount    = DescriptorCount,
            .stageFlags         = ShaderStages,
            .pImmutableSamplers = nullptr,
        });

        // Runtime-sized sampled texture arrays use Vulkan variable descriptor
        // count and update-after-bind flags.
        auto Flags = vk::DescriptorBindingFlags{};
        if (bUnboundedArray && Binding.Type == ShaderResourceType::SampledTexture) {
            Flags = vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                    vk::DescriptorBindingFlagBits::ePartiallyBound |
                    vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
            bHasBindingFlags = true;
        }
        BindingFlags.push_back(Flags);
    }

    if (bHasBindingFlags) {
        // Any binding using UPDATE_AFTER_BIND requires the layout-level
        // eUpdateAfterBindPool flag and the per-binding flags pNext chain.
        vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsCI{
            .bindingCount  = static_cast<Uint32>(BindingFlags.size()),
            .pBindingFlags = BindingFlags.data(),
        };
        vk::StructureChain<vk::DescriptorSetLayoutCreateInfo, vk::DescriptorSetLayoutBindingFlagsCreateInfo>
             LayoutChain = {
                 {.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                  .bindingCount = static_cast<Uint32>(VkBindings.size()),
                  .pBindings    = VkBindings.data()},
                 FlagsCI,
             };
        auto Result = Device.createDescriptorSetLayout(LayoutChain.get<vk::DescriptorSetLayoutCreateInfo>());
        if (Result.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create reflected descriptor set layout"));
        return std::move(Result.value);
    }

    vk::DescriptorSetLayoutCreateInfo LayoutCI{
        .bindingCount = static_cast<Uint32>(VkBindings.size()),
        .pBindings    = VkBindings.empty() ? nullptr : VkBindings.data(),
    };
    auto Result = Device.createDescriptorSetLayout(LayoutCI);
    if (Result.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("Failed to create reflected descriptor set layout"));
    return std::move(Result.value);
}

[[nodiscard]] auto CreatePipelineLayout(vk::raii::Device&             Device,
                                        const ShaderReflection&     Reflection,
                                        vk::ShaderStageFlags          ShaderStages = vk::ShaderStageFlagBits::eAllGraphics)
    -> std::expected<std::pair<std::vector<vk::raii::DescriptorSetLayout>, vk::raii::PipelineLayout>, ErrorMessage> {
    const Uint32 MaxBoundDescriptorSets = VulkanCapability::Get().GetProperties().limits.maxBoundDescriptorSets;
    Uint32 MaxSet = 0;
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Set >= MaxBoundDescriptorSets)
            return std::unexpected(ErrorMessage(Format(
                "Reflected binding '{}' uses set {} which exceeds the device limit of {} bound descriptor sets",
                Binding.ParameterPath,
                Binding.Set,
                MaxBoundDescriptorSets)));
        MaxSet = (std::max)(MaxSet, Binding.Set);
    }

    std::vector<std::vector<ShaderBinding>> BindingsBySet(MaxSet + 1);
    for (const auto& Binding : Reflection.Bindings)
        BindingsBySet[Binding.Set].push_back(Binding);
    for (auto& SetBindings : BindingsBySet) {
        std::sort(SetBindings.begin(),
                  SetBindings.end(),
                  [](const ShaderBinding& Left, const ShaderBinding& Right) -> bool {
                      return Left.BindingIndex < Right.BindingIndex;
                  });
    }

    std::vector<vk::raii::DescriptorSetLayout> SetLayouts;
    SetLayouts.reserve(BindingsBySet.size());
    for (const auto& SetBindings : BindingsBySet) {
        auto SetLayout = CreateDescriptorSetLayout(Device, SetBindings, ShaderStages);
        if (!SetLayout)
            return std::unexpected(SetLayout.error());
        SetLayouts.push_back(std::move(*SetLayout));
    }

    std::vector<vk::DescriptorSetLayout> RawSetLayouts;
    RawSetLayouts.reserve(SetLayouts.size());
    for (const auto& SetLayout : SetLayouts)
        RawSetLayouts.push_back(*SetLayout);

    std::vector<vk::PushConstantRange> PushConstants;
    PushConstants.reserve(Reflection.PushConstants.size());
    for (const auto& Range : Reflection.PushConstants) {
        PushConstants.push_back(vk::PushConstantRange{
            .stageFlags = ShaderStages,
            .offset     = Range.Offset,
            .size       = Range.Size,
        });
    }

    vk::PipelineLayoutCreateInfo PipelineLayoutCI{
        .setLayoutCount         = static_cast<Uint32>(RawSetLayouts.size()),
        .pSetLayouts            = RawSetLayouts.empty() ? nullptr : RawSetLayouts.data(),
        .pushConstantRangeCount = static_cast<Uint32>(PushConstants.size()),
        .pPushConstantRanges    = PushConstants.empty() ? nullptr : PushConstants.data(),
    };
    auto PipelineLayout = Device.createPipelineLayout(PipelineLayoutCI);
    if (PipelineLayout.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("Failed to create reflected pipeline layout"));

    return std::pair{std::move(SetLayouts), std::move(PipelineLayout.value)};
}

[[nodiscard]] auto CountDynamicOffsets(const ShaderReflection& Reflection) -> Uint32 {
    Uint32 Count = 0;
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Type == ShaderResourceType::ConstantBuffer)
            ++Count;
    }
    return Count;
}

struct VulkanReflectedDescriptorBinding {
    String               ParameterPath      = {};
    Uint32               Set                = 0;
    Uint32               Binding            = 0;
    ShaderResourceType Type               = ShaderResourceType::Unknown;
    Uint32               ArrayCount         = 1;
    Uint32               DynamicOffsetIndex = std::numeric_limits<Uint32>::max();
};

struct VulkanDescriptorSetInstance {
    vk::raii::DescriptorSet                  Set                     = nullptr;
    Uint32                                   VariableDescriptorCount = 0;
    Uint64                                   ParameterRevision       = 0;
    bool                                     Initialized             = false;
    std::unordered_map<Uint32, const void*> ResourceBindings = {};
};

struct VulkanPipelineParameterSetInstances {
    std::unordered_map<Uint64, std::vector<std::vector<std::vector<VulkanDescriptorSetInstance>>>> ByParameterId = {};
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
        auto& Out          = Result.emplace_back();
        Out.ParameterPath  = Binding.ParameterPath;
        Out.Set            = Binding.Set;
        Out.Binding        = Binding.BindingIndex;
        Out.Type           = Binding.Type;
        Out.ArrayCount     = Binding.ArrayCount;
        if (Binding.Type == ShaderResourceType::ConstantBuffer)
            Out.DynamicOffsetIndex = DynamicOffsetIndex++;
    }
    return Result;
}

[[nodiscard]] auto MaxPushConstantSize(const ShaderReflection& Reflection) -> Uint32 {
    Uint32 Size = 0;
    for (const auto& Range : Reflection.PushConstants)
        Size = (std::max)(Size, Range.Offset + Range.Size);
    return Size;
}

// ═════════════════════════════════════════════════════════════════════════════
// VulkanGraphicsPipeline — concrete Vulkan pipeline owned by ResourceManager.
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan graphics pipeline with a shader-reflected pipeline layout.
/// Created via the static `Create` factory. RHICommand lists only observe it.
class VulkanGraphicsPipeline final : public RHIGraphicsPipeline {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    VulkanGraphicsPipeline() = default;

    ~VulkanGraphicsPipeline() override = default;

    VulkanGraphicsPipeline(const VulkanGraphicsPipeline&)                    = delete;
    auto operator=(const VulkanGraphicsPipeline&) -> VulkanGraphicsPipeline& = delete;

    /// Create a Vulkan graphics pipeline from an RHI descriptor.
    ///
    /// Uses a pipeline layout generated from pipeline-level shader reflection. Shader
    /// modules are transient — destroyed when this function returns.
    [[nodiscard]] static auto Create(const VulkanResourceContext&    Context,
                                     const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<UPtr<VulkanGraphicsPipeline>, ErrorMessage> {

        auto LayoutObjects = CreatePipelineLayout(Context.Device, Desc.Program.Reflection);
        if (!LayoutObjects)
            return std::unexpected(LayoutObjects.error().Append("Failed to create graphics pipeline layout"));

        // ── Shader stages ──────────────────────────────────────────────
        auto ShaderStates = VulkanGraphicsShaderStates::Create(Context.Device, Desc);
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
        vk::Format DepthVkFormat =
            HasDepth ? ToVkFormat(Desc.DepthFormat) : vk::Format::eUndefined;

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
             .layout              = *LayoutObjects->second,
             // using dynamic rendering rather than traditional render pass
             .renderPass          = nullptr},
            RenderingCI};

        auto [PipelineResult, RHIPipeline] =
            Context.Device.createGraphicsPipeline(nullptr, PipelineChain.get<vk::GraphicsPipelineCreateInfo>());
        if (PipelineResult != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("Failed to create graphics pipeline: {}", vk::to_string(PipelineResult))));

        auto Ret             = std::make_unique<VulkanGraphicsPipeline>();
        Ret->m_Pipeline           = std::make_shared<vk::raii::Pipeline>(std::move(RHIPipeline));
        Ret->m_SetLayouts         =
            std::make_shared<std::vector<vk::raii::DescriptorSetLayout>>(std::move(LayoutObjects->first));
        Ret->m_PipelineLayout     = std::make_shared<vk::raii::PipelineLayout>(std::move(LayoutObjects->second));
        Ret->m_DescriptorSetCount = static_cast<Uint32>(Ret->m_SetLayouts->size());
        Ret->m_RawSetLayouts.reserve(Ret->m_SetLayouts->size());
        for (const auto& SetLayout : *Ret->m_SetLayouts)
            Ret->m_RawSetLayouts.push_back(*SetLayout);
        Ret->m_DynamicOffsetCount = CountDynamicOffsets(Desc.Program.Reflection);
        Ret->m_Bindings           = BuildReflectedBindings(Desc.Program.Reflection);
        Ret->m_PushConstantSize   = MaxPushConstantSize(Desc.Program.Reflection);
        Ret->m_ColorFormats       = Desc.ColorFormats;
        Ret->m_DepthFormat        = Desc.DepthFormat;
        Ret->SetShaderParameterLayout(RHIShaderParameterLayout::Create(Desc.Program.Reflection));
        return Ret;
    }

    /// Return the native VkPipeline handle.
    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_Pipeline);
    }

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout {
        return *(*m_PipelineLayout);
    }

    [[nodiscard]] auto GetDescriptorSetCount() const -> Uint32 {
        return m_DescriptorSetCount;
    }

    [[nodiscard]] auto GetDescriptorSetLayouts() const -> std::span<const vk::DescriptorSetLayout> {
        return m_RawSetLayouts;
    }

    [[nodiscard]] auto GetDescriptorSetLayout(Uint32 Set) const -> vk::DescriptorSetLayout;

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

    [[nodiscard]] auto GetPushConstantSize() const -> Uint32 {
        return m_PushConstantSize;
    }

    [[nodiscard]] auto IsCompatibleWith(std::span<const RHIFormat> ColorFormats, RHIFormat DepthFormat) const -> bool {
        return std::ranges::equal(m_ColorFormats, ColorFormats) && m_DepthFormat == DepthFormat;
    }

    [[nodiscard]] auto GetOrCreateDescriptorSetInstance(Uint64             ParameterId,
                                                         Uint32             FrameIndex,
                                                         Uint32             SetIndex,
                                                         Uint32             VariableDescriptorCount,
                                                         VulkanDescriptorManager& Descriptors)
        -> std::expected<VulkanDescriptorSetInstance*, ErrorMessage>;

  private:
    SPtr<vk::raii::Pipeline>                         m_Pipeline           = nullptr;
    SPtr<vk::raii::PipelineLayout>                   m_PipelineLayout     = nullptr;
    SPtr<std::vector<vk::raii::DescriptorSetLayout>> m_SetLayouts         = nullptr;
    std::vector<vk::DescriptorSetLayout>             m_RawSetLayouts      = {};
    std::vector<VulkanReflectedDescriptorBinding>          m_Bindings           = {};
    SPtr<VulkanPipelineParameterSetInstances>              m_ParameterSets      = std::make_shared<VulkanPipelineParameterSetInstances>();
    Uint32                                           m_DescriptorSetCount = 0;
    Uint32                                           m_DynamicOffsetCount = 0;
    Uint32                                           m_PushConstantSize   = 0;
    std::vector<RHIFormat>                           m_ColorFormats       = {};
    RHIFormat                                        m_DepthFormat        = RHIFormat::Unknown;
};

auto VulkanGraphicsPipeline::GetDescriptorSetLayout(Uint32 Set) const -> vk::DescriptorSetLayout {
    if (Set >= m_RawSetLayouts.size())
        return nullptr;
    return m_RawSetLayouts[Set];
}

auto VulkanGraphicsPipeline::GetOrCreateDescriptorSetInstance(Uint64             ParameterId,
                                                         Uint32             FrameIndex,
                                                         Uint32             SetIndex,
                                                         Uint32             VariableDescriptorCount,
                                                         VulkanDescriptorManager& Descriptors)
    -> std::expected<VulkanDescriptorSetInstance*, ErrorMessage> {
    if (SetIndex >= m_RawSetLayouts.size())
        return std::unexpected(ErrorMessage(Format("Parameter set uses missing descriptor set {}", SetIndex)));

    auto& Frames = m_ParameterSets->ByParameterId[ParameterId];
    if (FrameIndex >= Descriptors.GetFramesInFlight())
        return std::unexpected(ErrorMessage(Format("Invalid frame index {} for descriptor set instance", FrameIndex)));
    if (Frames.size() < Descriptors.GetFramesInFlight())
        Frames.resize(Descriptors.GetFramesInFlight());

    auto& Instances = Frames[FrameIndex];
    if (Instances.size() < m_RawSetLayouts.size())
        Instances.resize(m_RawSetLayouts.size());

    auto& Versions = Instances[SetIndex];
    for (auto& Instance : Versions) {
        if (Instance.VariableDescriptorCount == VariableDescriptorCount)
            return &Instance;
    }

    auto Set = Descriptors.AllocatePersistentDescriptorSet(m_RawSetLayouts[SetIndex], VariableDescriptorCount);
    if (!Set)
        return std::unexpected(Set.error());

    Versions.push_back(VulkanDescriptorSetInstance{
        .Set                     = std::move(*Set),
        .VariableDescriptorCount = VariableDescriptorCount,
    });
    return &Versions.back();
}

} // namespace SoulEngine
