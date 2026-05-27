module;

export module Vulkan:CommandList;

import RHI;

import :Types;
import :Swapchain;
import :Buffer;
import :Descriptor;
import :Pipeline;

import std;
import vulkan;

export namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// CommandList
// ═════════════════════════════════════════════════════════════════════════════

class CommandList final : public RHI::CommandList {
  public:
    explicit CommandList(vk::raii::Device& Device, DescriptorManager& Manager)
        : m_Device(Device), m_DescriptorManager(Manager) {}

    ~CommandList() override = default;

    CommandList(const CommandList&)                    = delete;
    auto operator=(const CommandList&) -> CommandList& = delete;
    CommandList(CommandList&&)                         = delete;
    auto operator=(CommandList&&) -> CommandList&      = delete;

    /// @brief Set the active command buffer for recording this frame.
    /// Called by RenderDevice before Begin() each frame.
    ///
    /// We need to store a pointer to vk::raii structure, not vk:: structure,
    /// becaude vk::raii strcture hold the dispatch table.
    auto SetActiveCommandBuffer(vk::raii::CommandBuffer& CmdBuf) -> void {
        m_ActiveCommandBuffer = &CmdBuf;
    }

    // ── CommandList overrides ────────────────────────────────────

    [[nodiscard]] auto Begin() -> std::expected<void, ErrorMessage> override {
        if (!m_ActiveCommandBuffer)
            return std::unexpected(
                ErrorMessage("No active command buffer set — call SetActiveCommandBuffer before Begin"));

        // ── Snapshot queue-committed image state for barrier tracking ────
        if (m_CommittedImageStates)
            m_LocalImageStates = *m_CommittedImageStates;

        // TODO: learn eOneTimeSubmit eRenderPassContinue eSimultaneousUse
        if (auto Result = m_ActiveCommandBuffer->begin({}); Result != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Core::Format("vkBeginCommandBuffer failed: {}", vk::to_string(Result))));

        // ── Bind global descriptor sets ──────────────────────────────────
        m_DescriptorManager.BindTo(*m_ActiveCommandBuffer);

        return {};
    }

    [[nodiscard]] auto End() -> std::expected<void, ErrorMessage> override {
        if (!m_ActiveCommandBuffer)
            return std::unexpected(ErrorMessage("No active command buffer set"));

        auto Result = m_ActiveCommandBuffer->end();
        if (Result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Core::Format("vkEndCommandBuffer failed: {}", vk::to_string(Result))));
        return {};
    }

    [[nodiscard]] auto Reset() -> std::expected<void, ErrorMessage> override {
        if (!m_ActiveCommandBuffer)
            return std::unexpected(
                ErrorMessage("No active command buffer set — call SetActiveCommandBuffer before Reset"));

        if (auto Result = m_ActiveCommandBuffer->reset({}); Result != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Core::Format("vkResetCommandBuffer failed: {}", vk::to_string(Result))));
        return {};
    }

    [[nodiscard]] auto BeginRendering(const RenderingDesc& Desc) -> std::expected<void, ErrorMessage> override {
        // Resolve image view: if Texture is NullHandle, use swapchain backbuffer
        vk::ImageView ImageView;
        if (Desc.ColorAttachment.Texture.Handle == 0) {
            if (!m_Swapchain)
                return std::unexpected(ErrorMessage("No swapchain available for backbuffer rendering"));
            ImageView = m_Swapchain->GetImageView(m_Swapchain->GetCurrentIndex());

            // Transition swapchain image to ColorAttachmentOptimal for rendering.
            // TransitionImage automatically determines whether a barrier is needed
            // by comparing the tracked state against the target.  First-frame usage
            // derives oldLayout=eUndefined from the default-constructed ImageState.
            TransitionImage(m_Swapchain->GetImage(m_Swapchain->GetCurrentIndex()),
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite,
                            vk::ImageLayout::eColorAttachmentOptimal,
                            true); // isWrite
        } else {
            // TODO: Look up texture from registry when Vulkan::Texture wrapper exists
            return std::unexpected(ErrorMessage("Non-swapchain color attachments not yet implemented"));
        }

        // ── Read swapchain extent for render area ───────────────────────────
        vk::Extent2D SwapchainExtent = m_Swapchain->GetExtent();

        vk::RenderingAttachmentInfo ColorAttachment{
            .imageView   = ImageView,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp      = vk::AttachmentLoadOp::eClear,
            .storeOp     = vk::AttachmentStoreOp::eStore,
            .clearValue  = vk::ClearValue{.color = vk::ClearColorValue(std::array<float, 4>{
                                              Desc.ColorAttachment.ClearValue.R,
                                              Desc.ColorAttachment.ClearValue.G,
                                              Desc.ColorAttachment.ClearValue.B,
                                              Desc.ColorAttachment.ClearValue.A,
                                          })},
        };

        vk::RenderingInfo RenderingInfo{
            .renderArea =
                vk::Rect2D{
                    .offset = vk::Offset2D{0, 0},
                    .extent = SwapchainExtent,
                },
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &ColorAttachment,
        };

        m_ActiveCommandBuffer->beginRendering(RenderingInfo);

        // ── Auto-apply the default viewport and scissor rectangle ────────
        {
            vk::Viewport VkVP{
                0.0f,
                0.0f,
                static_cast<float>(SwapchainExtent.width),
                static_cast<float>(SwapchainExtent.height),
                0.0f,
                1.0f,
            };
            m_ActiveCommandBuffer->setViewport(0, {VkVP});

            vk::Rect2D Scissor{
                .offset = {0, 0},
                .extent = SwapchainExtent,
            };
            m_ActiveCommandBuffer->setScissor(0, {Scissor});
        }

        m_IsRendering = true;
        return {};
    }

    [[nodiscard]] auto EndRendering() -> std::expected<void, ErrorMessage> override {
        if (!m_IsRendering)
            return std::unexpected(ErrorMessage("EndRendering called without an active rendering scope"));

        m_ActiveCommandBuffer->endRendering();
        m_IsRendering = false;

        // ── If rendering to swapchain backbuffer, transition to PresentSrcKHR ──
        if (m_Swapchain) {
            TransitionImage(m_Swapchain->GetImage(m_Swapchain->GetCurrentIndex()),
                            vk::PipelineStageFlagBits2::eBottomOfPipe,
                            vk::AccessFlagBits2::eNone,
                            vk::ImageLayout::ePresentSrcKHR,
                            false);
        }

        return {};
    }

    // ── Pipeline binding ────────────────────────────────────────────────

    [[nodiscard]] auto BindPipeline(const RHI::GraphicsPipeline& Pipe) -> std::expected<void, ErrorMessage> override {
        auto& VkPipe = static_cast<const Vulkan::GraphicsPipeline&>(Pipe);
        m_ActiveCommandBuffer->bindPipeline(vk::PipelineBindPoint::eGraphics, VkPipe.Get());
        return {};
    }

    // ── Vertex / index buffers ──────────────────────────────────────────

    [[nodiscard]] auto BindVertexBuffer(const RHI::VertexBuffer& VB, Uint64 Offset)
        -> std::expected<void, ErrorMessage> override {
        auto& VkBuf = static_cast<const Vulkan::VertexBuffer&>(VB);
        m_ActiveCommandBuffer->bindVertexBuffers(0, {VkBuf.GetVkBuffer()}, {Offset});
        return {};
    }

    [[nodiscard]] auto BindIndexBuffer(SPtr<RHI::IndexBuffer> IdxBuf, Uint64 Offset)
        -> std::expected<void, ErrorMessage> override {
        auto& VkBuf = static_cast<const Vulkan::IndexBuffer&>(*IdxBuf);
        m_ActiveCommandBuffer->bindIndexBuffer(VkBuf.GetVkBuffer(), Offset, VkBuf.GetIndexType());
        return {};
    }

    // ── Scissor rect ───────────────────────────────────────────────

    [[nodiscard]] auto SetScissorRect(Int32 X, Int32 Y, Uint32 Width, Uint32 Height)
        -> std::expected<void, ErrorMessage> override {
        vk::Rect2D Scissor{{X, Y}, {Width, Height}};
        m_ActiveCommandBuffer->setScissor(0, {Scissor});
        return {};
    }

    // ── Draw / dispatch ─────────────────────────────────────────────────

    [[nodiscard]] auto Draw(Uint32 VertexCount, Uint32 InstanceCount, Uint32 FirstVertex, Uint32 FirstInstance)
        -> std::expected<void, ErrorMessage> override {
        m_ActiveCommandBuffer->draw(VertexCount, InstanceCount, FirstVertex, FirstInstance);
        return {};
    }

    [[nodiscard]] auto
    DrawIndexed(Uint32 IndexCount, Uint32 InstanceCount, Uint32 FirstIndex, Int32 VertexOffset, Uint32 FirstInstance)
        -> std::expected<void, ErrorMessage> override {
        m_ActiveCommandBuffer->drawIndexed(IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance);
        return {};
    }

    [[nodiscard]] auto Dispatch(Uint32 GroupX, Uint32 GroupY, Uint32 GroupZ)
        -> std::expected<void, ErrorMessage> override {
        m_ActiveCommandBuffer->dispatch(GroupX, GroupY, GroupZ);
        return {};
    }

    // ── Image barrier tracking ──────────────────────────────────────────

    /// Transition `Image` to a new state, inserting a VkImageMemoryBarrier2
    /// when the tracked state actually differs.  Skips the barrier when the
    /// state matches the target *and* the previous access was read-only.
    /// Subresource range covers all mip levels and array layers.
    auto TransitionImage(vk::Image               Image,
                         vk::PipelineStageFlags2 DstStage,
                         vk::AccessFlags2        DstAccess,
                         vk::ImageLayout         DstLayout,
                         bool                    IsWrite,
                         vk::ImageAspectFlags    Aspect = vk::ImageAspectFlagBits::eColor) -> void {
        auto It      = m_LocalImageStates.find(Image);
        auto Current = (It != m_LocalImageStates.end()) ? It->second : ImageState{};

        // Conservative barrier: skip only when stage+access+layout all match
        // AND the previous access was read-only.
        bool NeedsBarrier = (Current.stage != DstStage) || (Current.access != DstAccess) ||
                            (Current.layout != DstLayout) || Current.isWrite;

        if (NeedsBarrier) {
            vk::ImageMemoryBarrier2 Barrier{
                .srcStageMask        = Current.stage,
                .srcAccessMask       = Current.access,
                .dstStageMask        = DstStage,
                .dstAccessMask       = DstAccess,
                .oldLayout           = Current.layout,
                .newLayout           = DstLayout,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image               = Image,
                .subresourceRange =
                    {
                        .aspectMask     = Aspect,
                        .baseMipLevel   = 0,
                        .levelCount     = vk::RemainingMipLevels,
                        .baseArrayLayer = 0,
                        .layerCount     = vk::RemainingArrayLayers,
                    },
            };
            vk::DependencyInfo Dep{
                .dependencyFlags         = vk::DependencyFlagBits::eByRegion,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &Barrier,
            };
            m_ActiveCommandBuffer->pipelineBarrier2(Dep);
        }

        // Always update the local state to reflect the new target state.
        m_LocalImageStates[Image] = ImageState{
            .stage       = DstStage,
            .access      = DstAccess,
            .layout      = DstLayout,
            .queueFamily = vk::QueueFamilyIgnored,
            .isWrite     = IsWrite,
        };
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Internals (exposed to RenderDevice for state commit)
    // ═════════════════════════════════════════════════════════════════════════

    auto SetSwapchain(Swapchain& Swc) -> void {
        m_Swapchain = &Swc;
    }

    /// @brief Wire up the queue-committed image state map.
    /// Called by VKRenderDevice::Init during engine startup.
    auto SetCommittedImageStates(std::unordered_map<vk::Image, ImageState>* Committed) -> void {
        m_CommittedImageStates = Committed;
    }

    /// @brief Read local image states for commit by VKRenderDevice::EndFrame.
    [[nodiscard]] auto GetLocalImageStates() const -> const std::unordered_map<vk::Image, ImageState>& {
        return m_LocalImageStates;
    }

  private:
    // ── Non-owning references ────────────────────────────────────────────

    vk::raii::Device&         m_Device;
    DescriptorManager&          m_DescriptorManager;

    // ── Borrowed resources (owned by RenderDevice) ──────────────────────

    vk::raii::CommandBuffer* m_ActiveCommandBuffer = nullptr;
    bool                     m_IsRendering         = false;

    // ── Non-owning external state ────────────────────────────────────────

    Swapchain* m_Swapchain = nullptr;

    // ── Per-command-list barrier state tracking ──────────────────────────

    /// Non-owning pointer to the queue-committed image state map held by
    /// VKRenderDevice.  Snapshot at Begin(), written back at commit time
    /// (during EndFrame).
    std::unordered_map<vk::Image, ImageState>* m_CommittedImageStates = nullptr;

    /// Local working copy of image states for this recording session.
    /// Seeded from m_CommittedImageStates at Begin, updated on every
    /// TransitionImage call.
    std::unordered_map<vk::Image, ImageState> m_LocalImageStates;

    /// TODO: m_LocalBufferStates for buffer barrier tracking.
};

} // namespace SoulEngine::RHI::Vulkan
