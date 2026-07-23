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
import :DeletionQueue;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

[[nodiscard]] auto ToVkDescriptorType(const Shader::Binding& Binding)
    -> std::expected<vk::DescriptorType, ErrorMessage> {
    switch (Binding.Type) {
    case Shader::ResourceType::ConstantBuffer:
        return vk::DescriptorType::eUniformBufferDynamic;
    case Shader::ResourceType::StorageBuffer:
        return vk::DescriptorType::eStorageBuffer;
    case Shader::ResourceType::SampledTexture:
        return vk::DescriptorType::eSampledImage;
    case Shader::ResourceType::StorageTexture:
        return vk::DescriptorType::eStorageImage;
    case Shader::ResourceType::Sampler:
        return vk::DescriptorType::eSampler;
    case Shader::ResourceType::AccelerationStructure:
        return vk::DescriptorType::eAccelerationStructureKHR;
    case Shader::ResourceType::Unknown:
        break;
    }
    return std::unexpected(
        ErrorMessage(Core::Format("Unsupported reflected resource type for '{}'", Binding.ParameterPath)));
}

[[nodiscard]] auto CreateDescriptorSetLayout(vk::raii::Device&                Device,
                                             std::span<const Shader::Binding> Bindings,
                                             Uint32                            MaxTextures,
                                             vk::ShaderStageFlags              ShaderStages)
    -> std::expected<vk::raii::DescriptorSetLayout, ErrorMessage> {
    // Pipeline layouts are generated from linked shader reflection. The reflected
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
            (Binding.Type != Shader::ResourceType::SampledTexture || BindingIndex + 1 != Bindings.size())) {
            return std::unexpected(ErrorMessage(Core::Format(
                "Reflected runtime array '{}' must be the final sampled-texture binding in its descriptor set",
                Binding.ParameterPath)));
        }
        const Uint32 DescriptorCount = bUnboundedArray ? MaxTextures : Binding.ArrayCount;
        if (DescriptorCount == 0)
            return std::unexpected(
                ErrorMessage(Core::Format("Reflected binding '{}' has zero descriptor count", Binding.ParameterPath)));

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
        if (bUnboundedArray && Binding.Type == Shader::ResourceType::SampledTexture) {
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
                                        const Shader::Reflection&     Reflection,
                                        Uint32                        MaxTextures,
                                        vk::ShaderStageFlags          ShaderStages = vk::ShaderStageFlagBits::eAllGraphics)
    -> std::expected<std::pair<std::vector<vk::raii::DescriptorSetLayout>, vk::raii::PipelineLayout>, ErrorMessage> {
    const Uint32 MaxBoundDescriptorSets = Capability::Get().GetProperties().limits.maxBoundDescriptorSets;
    Uint32 MaxSet = 0;
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Set >= MaxBoundDescriptorSets)
            return std::unexpected(ErrorMessage(Core::Format(
                "Reflected binding '{}' uses set {} which exceeds the device limit of {} bound descriptor sets",
                Binding.ParameterPath,
                Binding.Set,
                MaxBoundDescriptorSets)));
        MaxSet = (std::max)(MaxSet, Binding.Set);
    }

    std::vector<std::vector<Shader::Binding>> BindingsBySet(MaxSet + 1);
    for (const auto& Binding : Reflection.Bindings)
        BindingsBySet[Binding.Set].push_back(Binding);
    for (auto& SetBindings : BindingsBySet) {
        std::ranges::sort(SetBindings, {}, &Shader::Binding::BindingIndex);
    }

    std::vector<vk::raii::DescriptorSetLayout> SetLayouts;
    SetLayouts.reserve(BindingsBySet.size());
    for (const auto& SetBindings : BindingsBySet) {
        auto SetLayout = CreateDescriptorSetLayout(Device, SetBindings, MaxTextures, ShaderStages);
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

[[nodiscard]] auto CountDynamicOffsets(const Shader::Reflection& Reflection) -> Uint32 {
    Uint32 Count = 0;
    for (const auto& Binding : Reflection.Bindings) {
        if (Binding.Type == Shader::ResourceType::ConstantBuffer)
            ++Count;
    }
    return Count;
}

struct ReflectedDescriptorBinding {
    String               ParameterPath      = {};
    Uint32               Set                = 0;
    Uint32               Binding            = 0;
    Shader::ResourceType Type               = Shader::ResourceType::Unknown;
    Uint32               ArrayCount         = 1;
    Uint32               DynamicOffsetIndex = std::numeric_limits<Uint32>::max();
};

struct DescriptorSetInstance {
    vk::raii::DescriptorSet                  Set                     = nullptr;
    Uint32                                   VariableDescriptorCount = 0;
    Uint64                                   ParameterRevision       = 0;
    bool                                     Initialized             = false;
    std::unordered_map<Uint32, const void*> ResourceBindings = {};
};

struct PipelineParameterSetInstances {
    std::unordered_map<Uint64, std::vector<std::vector<DescriptorSetInstance>>> ByParameterId = {};
};

[[nodiscard]] auto BuildReflectedBindings(const Shader::Reflection& Reflection)
    -> std::vector<ReflectedDescriptorBinding> {
    std::vector<Shader::Binding> Sorted = Reflection.Bindings;
    std::ranges::sort(Sorted, [](const Shader::Binding& Lhs, const Shader::Binding& Rhs) {
        if (Lhs.Set != Rhs.Set)
            return Lhs.Set < Rhs.Set;
        return Lhs.BindingIndex < Rhs.BindingIndex;
    });

    std::vector<ReflectedDescriptorBinding> Result;
    Result.reserve(Sorted.size());
    Uint32 DynamicOffsetIndex = 0;
    for (const auto& Binding : Sorted) {
        auto& Out          = Result.emplace_back();
        Out.ParameterPath  = Binding.ParameterPath;
        Out.Set            = Binding.Set;
        Out.Binding        = Binding.BindingIndex;
        Out.Type           = Binding.Type;
        Out.ArrayCount     = Binding.ArrayCount;
        if (Binding.Type == Shader::ResourceType::ConstantBuffer)
            Out.DynamicOffsetIndex = DynamicOffsetIndex++;
    }
    return Result;
}

[[nodiscard]] auto MaxPushConstantSize(const Shader::Reflection& Reflection) -> Uint32 {
    Uint32 Size = 0;
    for (const auto& Range : Reflection.PushConstants)
        Size = (std::max)(Size, Range.Offset + Range.Size);
    return Size;
}

// ═════════════════════════════════════════════════════════════════════════════
// GraphicsPipeline — concrete Vulkan pipeline owned by Resource::Manager.
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan graphics pipeline with a shader-reflected pipeline layout.
/// Created via the static `Create` factory. Command lists only observe it.
class GraphicsPipeline final : public RHI::GraphicsPipeline {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    GraphicsPipeline() = default;

    ~GraphicsPipeline() override {
        if (m_DeletionQueue) {
            m_DeletionQueue->Enqueue(GetLastUsageToken(),
                                     [Pipeline = m_Pipeline,
                                      PipelineLayout = m_PipelineLayout,
                                      SetLayouts = m_SetLayouts,
                                      ParameterSets = m_ParameterSets]() {});
        }
    }

    GraphicsPipeline(const GraphicsPipeline&)                    = delete;
    auto operator=(const GraphicsPipeline&) -> GraphicsPipeline& = delete;

    /// Create a Vulkan graphics pipeline from an RHI descriptor.
    ///
    /// Uses a pipeline layout generated from pipeline-level shader reflection. Shader
    /// modules are transient — destroyed when this function returns.
    [[nodiscard]] static auto Create(vk::raii::Device&             Device,
                                     const GraphicsPipelineDesc&   Desc,
                                     Uint32                        MaxTextures,
                                     DeletionQueue&                Queue)
        -> std::expected<UPtr<GraphicsPipeline>, ErrorMessage> {

        auto LayoutObjects = CreatePipelineLayout(Device, Desc.Program.Reflection, MaxTextures);
        if (!LayoutObjects)
            return std::unexpected(LayoutObjects.error().Append("Failed to create graphics pipeline layout"));

        // ── Shader stages ──────────────────────────────────────────────
        auto ShaderStates = GraphicsShaderStates::Create(Device, Desc);
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

        bool                                    HasDepth = Desc.DepthFormat != RHI::Format::Unknown;
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

        vk::Format ColorVkFormat = SoulEngine::RHI::Vulkan::ToVkFormat(Desc.ColorFormat);
        vk::Format DepthVkFormat =
            HasDepth ? SoulEngine::RHI::Vulkan::ToVkFormat(Desc.DepthFormat) : vk::Format::eUndefined;

        // Dynamic rendering allows us to specify color, depth, stencil attachments directly
        // after the pipeline is created
        vk::PipelineRenderingCreateInfo RenderingCI{
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &ColorVkFormat,
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

        auto [PipelineResult, Pipeline] =
            Device.createGraphicsPipeline(nullptr, PipelineChain.get<vk::GraphicsPipelineCreateInfo>());
        if (PipelineResult != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Core::Format("Failed to create graphics pipeline: {}", vk::to_string(PipelineResult))));

        auto Ret             = std::make_unique<GraphicsPipeline>();
        Ret->m_Pipeline           = std::make_shared<vk::raii::Pipeline>(std::move(Pipeline));
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
        Ret->SetShaderParameterLayout(RHI::ShaderParameterLayout::Create(Desc.Program.Reflection));
        Ret->m_DeletionQueue      = &Queue;
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

    [[nodiscard]] auto GetBindings() const -> std::span<const ReflectedDescriptorBinding> {
        return m_Bindings;
    }

    [[nodiscard]] auto FindBinding(StringView ParameterPath, Shader::ResourceType Type) const
        -> const ReflectedDescriptorBinding* {
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

    [[nodiscard]] auto GetOrCreateDescriptorSetInstance(Uint64             ParameterId,
                                                         Uint32             SetIndex,
                                                         Uint32             VariableDescriptorCount,
                                                         DescriptorManager& Descriptors)
        -> std::expected<DescriptorSetInstance*, ErrorMessage>;

  private:
    SPtr<vk::raii::Pipeline>                         m_Pipeline           = nullptr;
    SPtr<vk::raii::PipelineLayout>                   m_PipelineLayout     = nullptr;
    SPtr<std::vector<vk::raii::DescriptorSetLayout>> m_SetLayouts         = nullptr;
    std::vector<vk::DescriptorSetLayout>             m_RawSetLayouts      = {};
    std::vector<ReflectedDescriptorBinding>          m_Bindings           = {};
    SPtr<PipelineParameterSetInstances>              m_ParameterSets      = std::make_shared<PipelineParameterSetInstances>();
    Uint32                                           m_DescriptorSetCount = 0;
    Uint32                                           m_DynamicOffsetCount = 0;
    Uint32                                           m_PushConstantSize   = 0;
    DeletionQueue*                                   m_DeletionQueue      = nullptr;
};

auto GraphicsPipeline::GetDescriptorSetLayout(Uint32 Set) const -> vk::DescriptorSetLayout {
    if (Set >= m_RawSetLayouts.size())
        return nullptr;
    return m_RawSetLayouts[Set];
}

auto GraphicsPipeline::GetOrCreateDescriptorSetInstance(Uint64             ParameterId,
                                                         Uint32             SetIndex,
                                                         Uint32             VariableDescriptorCount,
                                                         DescriptorManager& Descriptors)
    -> std::expected<DescriptorSetInstance*, ErrorMessage> {
    if (SetIndex >= m_RawSetLayouts.size())
        return std::unexpected(ErrorMessage(Core::Format("Parameter set uses missing descriptor set {}", SetIndex)));

    auto& Instances = m_ParameterSets->ByParameterId[ParameterId];
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

    Versions.push_back(DescriptorSetInstance{
        .Set                     = std::move(*Set),
        .VariableDescriptorCount = VariableDescriptorCount,
    });
    return &Versions.back();
}

} // namespace SoulEngine::RHI::Vulkan
