module;

// For VMA loading procedure, see:
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html
// Because we use `DynamicLoader` from vulkan headers, we define:
// dynamically fetching pointers using `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>
#include <vk_mem_alloc.h>

export module Vulkan:RenderDevice;

import RHI; // RenderPassList and RHI command types.
import TaskGraph;
import vulkan;
import std;

import :Swapchain;
import :SurfaceProvider;
import :Command;
import :Types;
import :Semaphore;
import :Buffer;
import :FrameContext;
import :Capability;
import :Descriptor;
import :ShaderBindingSet;
import :Pipeline;
import :RayTracingPipeline;
import :AccelerationStructure;
import :Sampler;
import :Texture;
import :Context;
import :Debug;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanRenderDevice
// ═════════════════════════════════════════════════════════════════════════════

class VulkanRenderDevice final : public RHIRenderDevice {
  public:
    VulkanRenderDevice() {}
    ~VulkanRenderDevice() {
        Shutdown();
    }

    [[nodiscard]] auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> override {
        // Read configuration from engine config
        const auto& Cfg  = ConfigManager::Get().GetConfig();
        m_FramesInFlight = Cfg.Render.FramesInFlight.value_or(m_FramesInFlight);

        auto Resources = VulkanResourceContext::Create(WindowSys, m_FramesInFlight);
        if (!Resources)
            return std::unexpected(Resources.error().Append("VulkanResourceContext creation failed"));
        // Resource factory contexts borrow RenderDevice-owned Vulkan services.
        // VulkanResourceContext now owns those services and remains borrowed
        // by the resource factories through this RenderDevice-held pointer.
        m_ResourceContext = std::move(*Resources);

        auto Semaphore =
            VulkanTimelineSemaphore::Create(
                m_ResourceContext->GetDevice(), &m_ResourceContext->GetDebugUtils(), "Internal/Semaphore/GraphicsTimeline");
        if (!Semaphore)
            return std::unexpected(Semaphore.error());
        m_Timeline = std::move(*Semaphore);

        // ── VulkanSwapchain ───────────────────────────────────────────────
        auto Swapchain = VulkanSwapchain::Create(*m_ResourceContext);
        if (!Swapchain)
            return std::unexpected(Swapchain.error());
        m_Swapchain = std::move(*Swapchain);

        // ── VulkanFrameContext ───────────────────────────────────────────────────
        // Each frame slot gets its own Pool, PrimaryBuffer, and SubPool.
        m_FrameContext.clear();
        m_FrameContext.reserve(m_FramesInFlight);
        for (uint32_t i = 0; i < m_FramesInFlight; ++i) {
            auto FCRes = VulkanFrameContext::Create(*m_ResourceContext, i);
            if (!FCRes)
                return std::unexpected(FCRes.error().Append("VulkanFrameContext creation failed"));
            m_FrameContext.push_back(std::move(*FCRes));
        }

        // ── Transient constant arena ─────────────────────────────────────
        const auto ConstantArenaCapacity = Cfg.RhiVulkan.ConstantArenaBufferSize.value_or(4096);
        auto       TransientUniformArena = VulkanTransientUniformArena::Create(
            "Renderer/Transient/UniformArena", ConstantArenaCapacity, *m_ResourceContext, m_FramesInFlight);
        if (!TransientUniformArena)
            return std::unexpected(TransientUniformArena.error().Append("VulkanTransientUniformArena creation failed"));
        m_TransientUniformArena = std::move(*TransientUniformArena);

        const auto TransientShaderStorageArenaCapacity =
            static_cast<Uint64>(Cfg.RhiVulkan.TransientShaderStorageBufferSize.value_or(64U * 1024U * 1024U));
        auto             TransientShaderStorageArena =
            VulkanTransientShaderStorageArena::Create("Renderer/Transient/ShaderStorageArena",
                                                      TransientShaderStorageArenaCapacity,
                                                      *m_ResourceContext,
                                                      m_FramesInFlight);
        if (!TransientShaderStorageArena) {
            return std::unexpected(
                TransientShaderStorageArena.error().Append("VulkanTransientShaderStorageArena creation failed"));
        }
        m_TransientShaderStorageArena = std::move(*TransientShaderStorageArena);

        if (auto Res = InitializeImGui(WindowSys); !Res)
            return std::unexpected(Res.error().Append("Vulkan Dear ImGui renderer initialization failed"));

        m_NeedsShutdown = true;
        return {};
    }

    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Vulkan;
    }

    // ── Frame lifecycle — private ─────────────────────────────────────

    auto RetireInFlightSubmissions() -> void {
        auto CompletedValue = m_Timeline.GetCurrentValue();
        if (!CompletedValue)
            return;
        while (!m_InFlightSubmissions.empty() &&
               m_InFlightSubmissions.front().CompletionTimelineValue <= *CompletedValue) {
            m_InFlightSubmissions.pop_front();
        }
    }

    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override {
        // CPU-GPU sync: wait for the timeline semaphore to reach the value
        // from N frames ago (when this slot was last signalled).
        uint64_t WaitValue = m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue;
        if (auto R = m_Timeline.Wait(WaitValue); !R)
            return std::unexpected(R.error().Append("BeginFrame: timeline wait failed"));

        // GPU done with this frame slot — safe to free scratch secondaries.
        m_FrameContext[m_CurrentFrame].ScratchSecondaries.clear();

        // Free any GPU resources whose transfer operations have completed.
        RetireInFlightSubmissions();

        auto& PresentCompleteSema = m_FrameContext[m_CurrentFrame].PresentComplete;
        auto  AcquireRes          = m_Swapchain.AcquireNextImage(PresentCompleteSema);
        while (AcquireRes == vk::Result::eErrorOutOfDateKHR || AcquireRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(
                    R.error().Append("VulkanSwapchain recreation failed after AcquireNextImage error"));

            auto NewSema = m_ResourceContext->GetDevice().createSemaphore({});
            if (NewSema.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create present-complete semaphore for retry"));
            m_FrameContext[m_CurrentFrame].PresentComplete = std::move(NewSema.value);
            AcquireRes = m_Swapchain.AcquireNextImage(m_FrameContext[m_CurrentFrame].PresentComplete);
        }
        if (AcquireRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(
                Format("AcquireNextImage failed after swapchain recreation: {}", vk::to_string(AcquireRes))));
        }

        // ── Begin primary command buffer (skeleton — only ExecuteCommands) ──
        auto& Primary = m_FrameContext[m_CurrentFrame].PrimaryBuffer;
        Primary.reset({});
        vk::CommandBufferBeginInfo PrimaryBegin{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        if (auto R = Primary.begin(PrimaryBegin); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("BeginFrame: primary CB begin failed: {}", vk::to_string(R))));

        if (auto R = m_TransientUniformArena.Reset(m_CurrentFrame); !R)
            return std::unexpected(R.error().Append("BeginFrame: transient constant arena reset failed"));
        if (auto R = m_TransientShaderStorageArena.Reset(m_CurrentFrame); !R)
            return std::unexpected(R.error().Append("BeginFrame: transient shader storage arena reset failed"));

        return {};
    }

    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return m_CurrentFrame;
    }

    [[nodiscard]] auto CreateVertexBuffer(StringView Name, const RHIVertexBufferDesc& Desc)
        -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> override {
        std::vector<std::byte> Data(Desc.Data.begin(), Desc.Data.end());

        return EnqueueResourceCreation<RHIVertexBuffer>(
            [this, Name = String(Name), Data = std::move(Data), VertexCount = Desc.VertexCount, Stride = Desc.Stride](
                RHIRef<RHIVertexBuffer>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Payload = Resource.m_Payload;
                auto Result  = VulkanVertexBuffer::Create(*m_ResourceContext,
                                                          Name,
                                                          RHIVertexBufferDesc{.Data = std::span<const std::byte>{Data},
                                                                              .VertexCount = VertexCount,
                                                                              .Stride      = Stride},
                                                          [Payload] { (void)Payload->TryMarkReady(); });
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                UPtr<RHIVertexBuffer> PayloadResource = std::move(*Result);
                return Resource.Publish(std::move(PayloadResource), RHIRefState::GpuPending);
            });
    }

    [[nodiscard]] auto CreateIndexBuffer(StringView Name, const RHIIndexBufferDesc& Desc)
        -> std::expected<RHIRef<RHIIndexBuffer>, ErrorMessage> override {
        std::vector<std::byte> Data(Desc.Data.begin(), Desc.Data.end());

        return EnqueueResourceCreation<RHIIndexBuffer>(
            [this, Name = String(Name), Data = std::move(Data), IndexCount = Desc.IndexCount](
                RHIRef<RHIIndexBuffer>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Payload = Resource.m_Payload;
                auto Result  = VulkanIndexBuffer::Create(
                    *m_ResourceContext,
                    Name,
                    RHIIndexBufferDesc{.Data = std::span<const std::byte>{Data}, .IndexCount = IndexCount},
                    [Payload] { (void)Payload->TryMarkReady(); });
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                UPtr<RHIIndexBuffer> PayloadResource = std::move(*Result);
                return Resource.Publish(std::move(PayloadResource), RHIRefState::GpuPending);
            });
    }

    [[nodiscard]] auto CreateTransientConstantBuffer(StringView                         Name,
                                                      const RHITransientConstantBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientConstantBuffer>, ErrorMessage> override {
        using magic_enum::bitwise_operators::operator|;

        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("Transient constant buffer data must not be empty"));

        auto Resource = RHIRef<RHITransientConstantBuffer>::Create();
        auto Data = std::vector<std::byte>{Desc.Data.begin(), Desc.Data.end()};

        auto TaskRef = Resource;
        auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(
            ThreadQueue::RHI,
            [this, TaskRef = std::move(TaskRef), Data = std::move(Data), Name = String(Name)] mutable {
                auto Offset = m_TransientUniformArena.Upload(std::span<const std::byte>{Data});
                if (!Offset) {
                    auto Error = Offset.error().Append("Transient constant buffer upload failed");
                    TaskRef.MarkFailed(Error);
                    m_TransientUploadError = std::move(Error);
                    return;
                }
                auto Payload = std::make_unique<VulkanTransientConstantBuffer>(
                    std::move(Name), Data.size(), *Offset, m_TransientUniformArena);
                if (auto Publish = TaskRef.Publish(std::move(Payload), RHIRefState::Ready); !Publish) {
                    auto Error = Publish.error().Append("Transient constant buffer publication failed");
                    TaskRef.MarkFailed(Error);
                    m_TransientUploadError = std::move(Error);
                    return;
                }

                m_PendingTransientBufferUsage =
                    m_PendingTransientBufferUsage | RHITransientBufferUsage::UniformRead;
            });
        if (!EnqueueResult) {
            Resource.MarkFailed(EnqueueResult.error());
            return std::unexpected(EnqueueResult.error());
        }
        return Resource;
    }

    [[nodiscard]] auto CreateTransientShaderStorageBuffer(
        StringView                              Name,
        const RHITransientShaderStorageBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientShaderStorageBuffer>, ErrorMessage> override {
        using magic_enum::bitwise_operators::operator|;

        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("Transient shader storage buffer data must not be empty"));
        if (Desc.Usage == RHITransientBufferUsage::Unknown)
            return std::unexpected(ErrorMessage("Transient shader storage buffer usage must not be unknown"));

        auto Resource = RHIRef<RHITransientShaderStorageBuffer>::Create();
        auto Data = std::vector<std::byte>{Desc.Data.begin(), Desc.Data.end()};

        auto TaskRef = Resource;
        auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(
            ThreadQueue::RHI,
            [this,
             TaskRef = std::move(TaskRef),
             Data = std::move(Data),
             Name = String(Name),
             Usage = Desc.Usage] mutable {
                auto Offset = m_TransientShaderStorageArena.Upload(std::span<const std::byte>{Data});
                if (!Offset) {
                    auto Error = Offset.error().Append("Transient shader storage buffer upload failed");
                    TaskRef.MarkFailed(Error);
                    m_TransientUploadError = std::move(Error);
                    return;
                }
                auto Payload = std::make_unique<VulkanTransientShaderStorageBuffer>(
                    std::move(Name), Data.size(), Usage, *Offset, m_TransientShaderStorageArena);
                if (auto Publish = TaskRef.Publish(std::move(Payload), RHIRefState::Ready); !Publish) {
                    auto Error = Publish.error().Append("Transient shader storage buffer publication failed");
                    TaskRef.MarkFailed(Error);
                    m_TransientUploadError = std::move(Error);
                    return;
                }

                m_PendingTransientBufferUsage =
                    m_PendingTransientBufferUsage | Usage;
            });
        if (!EnqueueResult) {
            Resource.MarkFailed(EnqueueResult.error());
            return std::unexpected(EnqueueResult.error());
        }
        return Resource;
    }

    [[nodiscard]] auto CreateSampler(StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> override {
        return EnqueueResourceCreation<RHISampler>(
            [this, Name = String(Name), Desc](RHIRef<RHISampler>& Resource) -> std::expected<void, ErrorMessage> {
                auto Result = VulkanSampler::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                UPtr<RHISampler> PayloadResource = std::move(*Result);
                return Resource.Publish(std::move(PayloadResource), RHIRefState::Ready);
            });
    }

    [[nodiscard]] auto CreateSampledTexture(StringView Name, const RHISampledTextureDesc& Desc)
        -> std::expected<RHIRef<RHISampledTexture>, ErrorMessage> override {
        // Sampled images upload on the dedicated transfer lane, then complete through a graphics-lane bridge.
        std::vector<std::byte> Data(Desc.Data.begin(), Desc.Data.end());

        return EnqueueResourceCreation<RHISampledTexture>(
            [this,
             Name     = String(Name),
             Data     = std::move(Data),
             Width    = Desc.Width,
             Height   = Desc.Height,
             Channels = Desc.Channels,
             Format   = Desc.Format,
             Usage    = Desc.Usage](RHIRef<RHISampledTexture>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Payload = Resource.m_Payload;
                auto Result =
                    VulkanSampledTexture::Create(*m_ResourceContext,
                                                 Name,
                                                 RHISampledTextureDesc{.Data     = std::span<const std::byte>{Data},
                                                                       .Width    = Width,
                                                                       .Height   = Height,
                                                                       .Channels = Channels,
                                                                       .Format   = Format,
                                                                       .Usage    = Usage},
                                                 [Payload] { (void)Payload->TryMarkReady(); });
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                UPtr<RHISampledTexture> PayloadResource = std::move(*Result);
                return Resource.Publish(std::move(PayloadResource), RHIRefState::GpuPending);
            });
    }

    [[nodiscard]] auto CreateRenderTarget(StringView Name, const RHIRenderTargetDesc& Desc)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> override {
        return EnqueueResourceCreation<RHIRenderTarget>(
            [this, Name = String(Name), Desc](RHIRef<RHIRenderTarget>& Resource) -> std::expected<void, ErrorMessage> {
                auto Result = VulkanRenderTarget::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                UPtr<RHIRenderTarget> PayloadResource = std::move(*Result);
                return Resource.Publish(std::move(PayloadResource), RHIRefState::Ready);
            });
    }

    [[nodiscard]] auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override {
        if (!m_ResourceContext)
            return std::unexpected(ErrorMessage("Vulkan shader binding set requires resource context"));

        auto Created = VulkanShaderBindingSet::Create(*m_ResourceContext, Name, Desc);
        if (!Created)
            return std::unexpected(Created.error());

        auto Resource = RHIRef<RHIShaderBindingSet>::Create();
        UPtr<RHIShaderBindingSet> Payload = std::move(*Created);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        auto Result = VulkanGraphicsPipeline::Create(*m_ResourceContext, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto                      Resource = RHIRef<RHIGraphicsPipeline>::Create();
        UPtr<RHIGraphicsPipeline> Payload  = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override {
        auto Result = VulkanRayTracingPipeline::Create(*m_ResourceContext, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto                        Resource = RHIRef<RHIRayTracingPipeline>::Create();
        UPtr<RHIRayTracingPipeline> Payload  = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(StringView                                     Name,
                                                              const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return EnqueueResourceCreation<RHIBottomLevelAccelerationStructure>(
            [this, Name = String(Name), Desc](
                RHIRef<RHIBottomLevelAccelerationStructure>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Result = VulkanBottomLevelAccelerationStructure::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                return Resource.Publish(std::move(*Result), RHIRefState::Ready);
            });
    }

    [[nodiscard]] auto CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                           const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return EnqueueResourceCreation<RHITopLevelAccelerationStructure>(
            [this, Name = String(Name), Desc](
                RHIRef<RHITopLevelAccelerationStructure>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Result = VulkanTopLevelAccelerationStructure::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                return Resource.Publish(std::move(*Result), RHIRefState::Ready);
            });
    }

    auto Tick() -> void override {
        m_ResourceContext->GetImmediateContext().Tick();
    }

    auto WaitIdle() -> void override {
        if (m_ResourceContext && *m_ResourceContext->GetDevice())
            (void)m_ResourceContext->GetDevice().waitIdle();
    }

    auto Shutdown() -> void override {
        if (std::exchange(m_NeedsShutdown, false)) {
            WaitIdle();
            ImGui_ImplVulkan_Shutdown();
            auto ImmediateDrain = m_ResourceContext->GetImmediateContext().Drain();
            if (!ImmediateDrain)
                LogError("{}", ImmediateDrain.error().ToString());
            WaitIdle();
            m_InFlightSubmissions.clear();
            DrainRHIDeferredDeletions();
        }

        // Destroy VMA-backed buffers before vmaDestroyAllocator.
        m_Swapchain.Cleanup();
        m_Timeline              = {};
        m_TransientShaderStorageArena = {};
        m_TransientUniformArena       = {};
        m_FrameContext.clear();

        m_ResourceContext.reset();
    }

  private:
    [[nodiscard]] auto EmitTransientUploadBarrier() -> std::expected<void, ErrorMessage> {
        using magic_enum::bitwise_operators::operator&;

        if (m_PendingTransientBufferUsage == RHITransientBufferUsage::Unknown)
            return {};
        const auto PendingUsage = std::exchange(
            m_PendingTransientBufferUsage, RHITransientBufferUsage::Unknown);
        vk::AccessFlags2 DestinationAccess = {};
        if ((PendingUsage & RHITransientBufferUsage::UniformRead) != RHITransientBufferUsage::Unknown)
            DestinationAccess |= vk::AccessFlagBits2::eUniformRead;
        if ((PendingUsage & RHITransientBufferUsage::ShaderRead) != RHITransientBufferUsage::Unknown)
            DestinationAccess |= vk::AccessFlagBits2::eShaderRead;
        if ((PendingUsage & RHITransientBufferUsage::IndirectCommandRead) != RHITransientBufferUsage::Unknown)
            DestinationAccess |= vk::AccessFlagBits2::eIndirectCommandRead;
        const vk::MemoryBarrier2 Barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = DestinationAccess,
        };
        const vk::DependencyInfo Dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &Barrier};
        m_FrameContext[m_CurrentFrame].PrimaryBuffer.pipelineBarrier2(Dependency);
        return {};
    }

    [[nodiscard]] auto InitializeImGui(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> {
        switch (WindowSys->GetType()) {
        case WindowSystemType::Glfw: {
            auto* Window = static_cast<GlfwWindowSystem*>(WindowSys)->GetNativeHandle();
            if (!ImGui_ImplGlfw_InitForVulkan(Window, true))
                return std::unexpected(ErrorMessage("ImGui GLFW platform backend initialization failed"));
            break;
        }
        case WindowSystemType::Unknown:
            return std::unexpected(ErrorMessage("Vulkan Dear ImGui renderer cannot use an unknown window system"));
        default:
            return std::unexpected(ErrorMessage("Vulkan Dear ImGui renderer does not support this window system"));
        }

        // Dear ImGui exposes a C Vulkan API. These raw values are confined to
        // this third-party adapter boundary.
        const VkFormat                ColorAttachmentFormat = static_cast<VkFormat>(m_Swapchain.GetFormat().format);
        VkPipelineRenderingCreateInfo PipelineRenderingCI{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &ColorAttachmentFormat,
        };
        ImGui_ImplVulkan_InitInfo InitInfo{};
        InitInfo.ApiVersion          = VK_API_VERSION_1_4;
        InitInfo.Instance            = static_cast<VkInstance>(*m_ResourceContext->GetInstance());
        InitInfo.PhysicalDevice      = static_cast<VkPhysicalDevice>(*m_ResourceContext->GetPhysicalDevice());
        InitInfo.Device              = static_cast<VkDevice>(*m_ResourceContext->GetDevice());
        InitInfo.QueueFamily         = m_ResourceContext->GetGraphicsFamily();
        InitInfo.Queue               = static_cast<VkQueue>(*m_ResourceContext->GetGraphicsQueue());
        InitInfo.DescriptorPool =
            static_cast<VkDescriptorPool>(m_ResourceContext->GetDescriptorManager().GetDescriptorPool());
        InitInfo.MinImageCount       = m_Swapchain.GetImageCount();
        InitInfo.ImageCount          = m_Swapchain.GetImageCount();
        InitInfo.UseDynamicRendering = true;
        InitInfo.PipelineInfoMain.PipelineRenderingCreateInfo = PipelineRenderingCI;

        if (!ImGui_ImplVulkan_LoadFunctions(
                VK_API_VERSION_1_4,
                [](const char* FunctionName, void* UserData) -> PFN_vkVoidFunction {
                    auto*            Instance    = static_cast<vk::raii::Instance*>(UserData);
                    const VkInstance RawInstance = static_cast<VkInstance>(**Instance);
                    return Instance->getDispatcher()->vkGetInstanceProcAddr(RawInstance, FunctionName);
                },
                &m_ResourceContext->GetInstance()))
            return std::unexpected(ErrorMessage("ImGui_ImplVulkan_LoadFunctions failed"));
        if (!ImGui_ImplVulkan_Init(&InitInfo))
            return std::unexpected(ErrorMessage("ImGui_ImplVulkan_Init failed"));

        return {};
    }

    template <typename T, typename CreateFn>
    [[nodiscard]] auto EnqueueResourceCreation(RHIRef<T> Resource, CreateFn&& Create)
        -> std::expected<void, ErrorMessage> {
        auto TaskRef       = Resource;
        auto EnqueueResult = TaskGraph::Get().EnqueueTask(
            ThreadQueue::RHI, [TaskRef = std::move(TaskRef), Create = std::forward<CreateFn>(Create)] mutable {
                if (auto Result = Create(TaskRef); !Result)
                    LogError("Failed to create RHI resource: {}", Result.error().ToString());
            });
        if (!EnqueueResult) {
            Resource.MarkFailed(EnqueueResult.error());
            return std::unexpected(EnqueueResult.error());
        }
        return {};
    }

    template <typename T, typename CreateFn>
    [[nodiscard]] auto EnqueueResourceCreation(CreateFn&& Create) -> std::expected<RHIRef<T>, ErrorMessage> {
        auto Resource = RHIRef<T>::Create();
        if (auto Result = EnqueueResourceCreation(Resource, std::forward<CreateFn>(Create)); !Result)
            return std::unexpected(Result.error());
        return Resource;
    }
    auto RecordPresentBlit(vk::raii::CommandBuffer& Buf, RHIRenderTarget* Source) -> void {
        auto& SrcRT = static_cast<const VulkanRenderTarget&>(*Source);

        const auto SrcImage = SrcRT.GetVkImage();
        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());

        m_ResourceContext->GetImageTracker().Transition(
            Buf,
            SrcImage,
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eTransfer,
                .access = vk::AccessFlagBits2::eTransferRead,
                .layout = vk::ImageLayout::eTransferSrcOptimal,
                .aspect = vk::ImageAspectFlagBits::eColor,
            });
        m_ResourceContext->GetImageTracker().Transition(
            Buf,
            DstImage,
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eTransfer,
                .access = vk::AccessFlagBits2::eTransferWrite,
                .layout = vk::ImageLayout::eTransferDstOptimal,
                .aspect = vk::ImageAspectFlagBits::eColor,
                .isWrite = true,
            });

        const auto DstExtent = m_Swapchain.GetExtent();
        vk::ImageBlit BlitRegion{
            .srcSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                               .mipLevel       = 0,
                               .baseArrayLayer = 0,
                               .layerCount     = 1},
            .srcOffsets =
                std::array<vk::Offset3D, 2>{
                    vk::Offset3D{0, 0, 0},
                    vk::Offset3D{static_cast<Int32>(SrcRT.GetWidth()), static_cast<Int32>(SrcRT.GetHeight()), 1}},
            .dstSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                               .mipLevel       = 0,
                               .baseArrayLayer = 0,
                               .layerCount     = 1},
            .dstOffsets =
                std::array<vk::Offset3D, 2>{
                    vk::Offset3D{0, 0, 0},
                    vk::Offset3D{static_cast<Int32>(DstExtent.width), static_cast<Int32>(DstExtent.height), 1}},
        };

        Buf.blitImage(SrcImage,
                      vk::ImageLayout::eTransferSrcOptimal,
                      DstImage,
                      vk::ImageLayout::eTransferDstOptimal,
                      {BlitRegion},
                      vk::Filter::eNearest);

    }

    [[nodiscard]] auto RecordImGuiPresentationOverlay(vk::raii::CommandBuffer&              Buf,
                                                      const RHIImGuiPresentationOverlayCmd& Overlay)
        -> std::expected<void, ErrorMessage> {
        if (!Overlay.Snapshot || !Overlay.TextureQueue || !Overlay.TextureMutex)
            return std::unexpected(ErrorMessage("ImGui presentation overlay has incomplete state"));
        {
            std::lock_guard Lock(*Overlay.TextureMutex);
            Overlay.TextureQueue->ProcessRequests(&Overlay.Snapshot->DrawData);
        }

        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());
        // The overlay follows the blit and alpha-blends over the scene, so it
        // must load and preserve the existing swapchain color.
        m_ResourceContext->GetImageTracker().Transition(
            Buf,
            DstImage,
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .access = vk::AccessFlagBits2::eColorAttachmentRead |
                          vk::AccessFlagBits2::eColorAttachmentWrite,
                .layout = vk::ImageLayout::eColorAttachmentOptimal,
            });

        const auto                  Extent = m_Swapchain.GetExtent();
        vk::RenderingAttachmentInfo ColorAttachment{
            .imageView   = m_Swapchain.GetCurrentImageView(),
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp      = vk::AttachmentLoadOp::eLoad,
            .storeOp     = vk::AttachmentStoreOp::eStore,
        };
        vk::RenderingInfo RenderingInfo{
            .renderArea           = vk::Rect2D{{0, 0}, Extent},
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &ColorAttachment,
        };
        Buf.beginRendering(RenderingInfo);
        ImGui_ImplVulkan_RenderDrawData(&Overlay.Snapshot->DrawData, static_cast<VkCommandBuffer>(*Buf));
        Buf.endRendering();

        return {};
    }

    // ═════════════════════════════════════════════════════════════════════════════
    // Execute — consume RenderPassList and record commands
    // ═════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] auto Execute(RenderPassList&& PassList) -> std::expected<void, ErrorMessage> override {
        if (m_TransientUploadError)
            return std::unexpected(std::exchange(m_TransientUploadError, std::nullopt).value());
        if (auto R = EmitTransientUploadBarrier(); !R)
            return std::unexpected(R.error().Append("Execute: transient upload barrier failed"));

        std::vector<std::function<void()>> RetiredPayloads;
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        for (std::size_t PassIndex = 0; PassIndex < PassList.Passes.size(); ++PassIndex) {
            const auto& Pass = PassList.Passes[PassIndex];
            if (!Pass)
                return std::unexpected(ErrorMessage(
                    Format("Execute: pass {} is null", PassIndex)));
            const auto& Pipeline = Pass->GetPipeline();
            const auto  PipelineState = std::visit(
                [](const auto& PipelineRef) -> RHIRefState {
                    using PipelineRefType = std::decay_t<decltype(PipelineRef)>;
                    if constexpr (std::same_as<PipelineRefType, std::monostate>)
                        return RHIRefState::Unknown;
                    else
                        return PipelineRef.GetState();
                },
                Pipeline);
            if (PipelineState == RHIRefState::RhiCommitting || PipelineState == RHIRefState::GpuPending)
                continue;
            if (PipelineState == RHIRefState::Failed) {
                const auto PipelineError = std::visit(
                    [](const auto& PipelineRef) -> std::optional<ErrorMessage> {
                        using PipelineRefType = std::decay_t<decltype(PipelineRef)>;
                        if constexpr (std::same_as<PipelineRefType, std::monostate>)
                            return std::nullopt;
                        else
                            return PipelineRef.GetError();
                    },
                    Pipeline);
                if (PipelineError)
                    return std::unexpected(PipelineError->Append(
                        Format("Execute: pass {} pipeline creation failed", PassIndex)));
                return std::unexpected(ErrorMessage(
                    Format("Execute: pass {} pipeline failed without an error", PassIndex)));
            }
            if (PipelineState != RHIRefState::Ready)
                return std::unexpected(ErrorMessage(
                    Format("Execute: pass {} has invalid pipeline state {}",
                           PassIndex,
                           magic_enum::enum_name(PipelineState))));
            if (auto R = Pass->Record(); !R)
                return std::unexpected(
                    R.error().Append(Format("Execute: pass {} recording failed", PassIndex)));

            // Allocate one secondary for this ordered rendering or non-rendering scope.
            vk::CommandBufferAllocateInfo Alloc{
                .commandPool        = *m_FrameContext[m_CurrentFrame].SubPool,
                .level              = vk::CommandBufferLevel::eSecondary,
                .commandBufferCount = 1,
            };
            auto AllocResult = m_ResourceContext->GetDevice().allocateCommandBuffers(Alloc);
            if (AllocResult.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Execute: failed to allocate secondary CB"));
                m_ResourceContext->GetDebugUtils().SetObjectName(
                    *AllocResult.value[0],
                Format("Internal/CommandBuffer/Secondary/Frame{}/Pass{}", m_CurrentFrame, PassIndex));
            auto& SecBuf =
                m_FrameContext[m_CurrentFrame].ScratchSecondaries.emplace_back(std::move(AllocResult.value[0]));

            vk::CommandBufferInheritanceInfo Inheritance{
                .renderPass           = nullptr,
                .subpass              = 0,
                .framebuffer          = nullptr,
                .occlusionQueryEnable = vk::False,
            };
            vk::CommandBufferBeginInfo BeginCI{
                .flags            = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
                .pInheritanceInfo = &Inheritance,
            };
            if (auto R = SecBuf.begin(BeginCI); R != vk::Result::eSuccess)
                return std::unexpected(
                    ErrorMessage(Format("Execute: secondary CB begin failed: {}", vk::to_string(R))));

            {
                auto                 ImageStateCopy = m_ResourceContext->GetImageTracker();
                const auto RecordCommands = [&](auto& Visitor) -> std::expected<void, ErrorMessage> {
                    if (Visitor.GetError())
                        return std::unexpected(*Visitor.GetError());
                    for (const auto& Cmd : Pass->GetCommands()) {
                        std::visit(Visitor, Cmd);
                        if (Visitor.GetError())
                            return std::unexpected(*Visitor.GetError());
                    }
                    return {};
                };
                auto RecordResult = std::visit(
                    [&](const auto& PipelineRef) -> std::expected<void, ErrorMessage> {
                        using PipelineRefType = std::decay_t<decltype(PipelineRef)>;
                        if constexpr (std::same_as<PipelineRefType, RHIRef<RHIGraphicsPipeline>>) {
                            if (Pass->GetType() != RHIPassType::Graphics)
                                return std::unexpected(
                                    ErrorMessage("Execute: pass type does not match graphics pipeline"));
                            const auto& GraphicsPass =
                                static_cast<const IRHIGraphicsPass&>(*Pass);
                            VulkanGraphicsCmdVisitor Visitor{
                                static_cast<VulkanGraphicsPipeline&>(*PipelineRef.TryGet()),
                                GraphicsPass.GetAttachments(),
                                SecBuf,
                                ImageStateCopy,
                                &RetiredPayloads,
                            };
                            return RecordCommands(Visitor);
                        } else if constexpr (std::same_as<PipelineRefType, RHIRef<RHIRayTracingPipeline>>) {
                            if (Pass->GetType() != RHIPassType::RayTracing)
                                return std::unexpected(
                                    ErrorMessage("Execute: pass type does not match ray-tracing pipeline"));
                            VulkanRayTracingCmdVisitor Visitor{
                                static_cast<VulkanRayTracingPipeline&>(*PipelineRef.TryGet()),
                                SecBuf,
                                ImageStateCopy,
                                &RetiredPayloads,
                            };
                            return RecordCommands(Visitor);
                        } else {
                            return std::unexpected(ErrorMessage("Execute: pass has no pipeline"));
                        }
                    },
                    Pass->GetPipeline());
                if (!RecordResult)
                    return std::unexpected(RecordResult.error());
                m_ResourceContext->GetImageTracker() = std::move(ImageStateCopy);
            }
            if (auto R = SecBuf.end(); R != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage(Format("Execute: secondary CB end failed: {}", vk::to_string(R))));

            Primary.executeCommands({static_cast<vk::CommandBuffer>(*SecBuf)});
            if (Pass->GetType() == RHIPassType::Graphics) {
                const auto& GraphicsPass = static_cast<const IRHIGraphicsPass&>(*Pass);
                if (GraphicsPass.HasPresentOutput()) {
                    const auto& Attachments = GraphicsPass.GetAttachments();
                    if (Attachments.ColorAttachments.size() != 1)
                        return std::unexpected(ErrorMessage(Format(
                            "Execute: pass {} present output requires exactly one color attachment", PassIndex)));
                    auto* PresentSource = Attachments.ColorAttachments.front().TextureRef.TryGet();
                    if (!PresentSource)
                        return std::unexpected(ErrorMessage(Format(
                            "Execute: pass {} present output attachment is not ready", PassIndex)));
                    RecordPresentBlit(Primary, PresentSource);
                }
            }
        }

        // Primary CB was already prepared in BeginFrame. Presentation overlays
        // follow all pass-local output blits and preserve the final swapchain contents.
        if (PassList.ImGuiPresentationOverlay) {
            if (auto R = RecordImGuiPresentationOverlay(Primary, *PassList.ImGuiPresentationOverlay); !R)
                return std::unexpected(R.error());
        }
        m_ResourceContext->GetImageTracker().Transition(
            Primary,
            m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex()),
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eTransfer,
                .layout = vk::ImageLayout::ePresentSrcKHR,
            });
        m_PendingCommandList = std::move(PassList);
        m_PendingRetiredPayloads = std::move(RetiredPayloads);
        return {};
    }

    [[nodiscard]] auto EndFrame() -> std::expected<void, ErrorMessage> override {
        if (!m_PendingCommandList)
            return std::unexpected(ErrorMessage("EndFrame called without a recorded frame"));

        auto PassList = std::move(*m_PendingCommandList);
        m_PendingCommandList.reset();

        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        // ── End primary ─────────────────────────────────────────────────
        if (auto R = Primary.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("EndFrame: primary CB end failed: {}", vk::to_string(R))));

        // ── Submit ──────────────────────────────────────────────────────────
        vk::CommandBufferSubmitInfo PrimarySubmitInfo{.commandBuffer = *Primary};

        vk::SemaphoreSubmitInfo     PresentCompleteSema{
            .semaphore = *m_FrameContext[m_CurrentFrame].PresentComplete,
            .stageMask = vk::PipelineStageFlagBits2::eTransfer,
        };
        vk::SemaphoreSubmitInfo RenderingCompleteSema{
            .semaphore = m_Swapchain.GetCurrentRenderCompleteSemaphore(),
            .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
        };

        auto TimelineSignalSema = m_Timeline.GetSignalSubmitInfo(vk::PipelineStageFlagBits2::eColorAttachmentOutput);

        vk::SemaphoreSubmitInfo SignalSemas[] = {
            RenderingCompleteSema,
            TimelineSignalSema,
        };

        vk::SubmitInfo2 SubmitInfo2{
            .waitSemaphoreInfoCount   = 1,
            .pWaitSemaphoreInfos      = &PresentCompleteSema,
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &PrimarySubmitInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos    = SignalSemas,
        };
        if (auto R = m_ResourceContext->GetGraphicsQueue().submit2({SubmitInfo2}); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("Queue submit failed: {}", vk::to_string(R))));

        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = TimelineSignalSema.value;

        m_InFlightSubmissions.push_back(VulkanInFlightSubmission{
            .CompletionTimelineValue = TimelineSignalSema.value,
            .CommandList             = std::move(PassList),
            .RetiredPayloads         = std::move(m_PendingRetiredPayloads),
        });

        // ── Present ────────────────────────────────────────────────────────
        auto PresentRes = m_Swapchain.Present(m_ResourceContext->GetGraphicsQueue());
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            if (auto R = m_Swapchain.Recreate(); !R)
                return std::unexpected(R.error().Append("VulkanSwapchain recreation failed after Present error"));
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
        return {};
    }

    // ── RAII resources ─────────────────────────────────────────────────────

    uint32_t m_FramesInFlight = 2;
    uint32_t m_CurrentFrame   = 0;

    // ── VulkanSwapchain & sync ──────────────────────────────────────────────────

    VulkanSwapchain         m_Swapchain;
    VulkanTimelineSemaphore m_Timeline;

    struct VulkanInFlightSubmission {
        Uint64                             CompletionTimelineValue = 0;
        RenderPassList                     CommandList             = {};
        std::vector<std::function<void()>> RetiredPayloads         = {};
    };
    std::deque<VulkanInFlightSubmission> m_InFlightSubmissions       = {};
    std::optional<RenderPassList>         m_PendingCommandList       = std::nullopt;
    std::vector<std::function<void()>>    m_PendingRetiredPayloads   = {};

    UPtr<VulkanResourceContext> m_ResourceContext = nullptr;

    std::vector<VulkanFrameContext>   m_FrameContext;
    VulkanTransientUniformArena       m_TransientUniformArena       = {};
    VulkanTransientShaderStorageArena m_TransientShaderStorageArena = {};
    RHITransientBufferUsage            m_PendingTransientBufferUsage = RHITransientBufferUsage::Unknown;
    std::optional<ErrorMessage>        m_TransientUploadError             = std::nullopt;
    bool m_NeedsShutdown = false;

};

} // namespace SoulEngine
