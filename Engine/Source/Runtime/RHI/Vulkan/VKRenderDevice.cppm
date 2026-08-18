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

import RHI; // RHICommandList, RHIPass, etc.
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
import :Pipeline;
import :RayTracingPipeline;
import :AccelerationStructure;
import :Sampler;
import :Texture;
import :Context;
import :Debug;

namespace SoulEngine {

namespace {

VKAPI_ATTR auto VKAPI_CALL VulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT      MessageSeverity,
                                               vk::DebugUtilsMessageTypeFlagsEXT             MessageTypes,
                                               const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
                                               void*) -> vk::Bool32 {
    if (!CallbackData) {
        LogError("[Vulkan] Debug callback data is empty");
        return vk::True;
    }
    const StringView MessageId = CallbackData->pMessageIdName ? CallbackData->pMessageIdName : "UnknownMessage";
    const StringView Message   = CallbackData->pMessage ? CallbackData->pMessage : "No Vulkan debug message";
    const auto       Types     = vk::to_string(MessageTypes);

    switch (MessageSeverity) {
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
        LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, Message);
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
        // There are too much `eInfo` messages,
        // so we use `LogDebug`.
        LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, Message);
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
        LogWarning("[Vulkan][{}][{}] {}", Types, MessageId, Message);
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
        LogError("[Vulkan][{}][{}] {}", Types, MessageId, Message);
        break;
    }
    return vk::False;
}

[[nodiscard]] auto CreateDebugMessengerCI() -> vk::DebugUtilsMessengerCreateInfoEXT {
    return vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity =
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = &VulkanDebugCallback,
    };
}

} // namespace

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
        auto SurfaceProvider = CreateVulkanSurfaceProvider(WindowSys);
        if (!SurfaceProvider)
            return std::unexpected(SurfaceProvider.error().Append("Failed to create Vulkan surface provider"));
        m_SurfaceProvider = std::move(*SurfaceProvider);

        // Read configuration from engine config
        const auto& Cfg  = ConfigManager::Get().GetConfig();
        m_FramesInFlight = Cfg.Render.FramesInFlight.value_or(m_FramesInFlight);

        // vk::raii::Context does:
        // 1. get `vkGetInstanceProcAddr`, whether dynamically or statically
        //    (dynamically means mechanism like dlopen)
        // 2. using `vkGetInstanceProcAddr` to collect all Instance Functions
        //
        // Once an instance is created, the instance will hold `vkGetInstanceProcAddr`,
        // thus the Context can be locally assigned.
        //
        // A `DynamicLoader` is hidden here, which searches for the loader from variable paths.
        //
        // TODO: Seg fault happens when loader is missing.
        //       Consider using `volk` instead.
        vk::raii::Context Context;

        if (auto Res = CreateInstance(Context, *m_SurfaceProvider); !Res.has_value())
            return std::unexpected(Res.error());

        // Surface must exist before PickPhysicalDevice so we can verify
        // surface presentation support via getSurfaceSupportKHR.
        auto Surface = m_SurfaceProvider->CreateSurface(m_Instance);
        if (!Surface)
            return std::unexpected(Surface.error().Append("Failed to create Vulkan presentation surface"));
        m_Surface = std::move(*Surface);

        if (auto Res = PickPhysicalDevice(); !Res.has_value())
            return std::unexpected(Res.error());

        if (auto Res = CreateLogicalDevice(); !Res.has_value())
            return std::unexpected(Res.error());
        m_DebugUtils.Initialize(m_Device);

        // ── VMA ───────────────────────────────────────────────────────────
        if (auto Res = CreateVMA(Context); !Res.has_value())
            return std::unexpected(Res.error());

        // ── Transfer Completion Queue ──────────────────────────────────────
        // Must be created before VulkanImmediateContext — VulkanImmediateContext borrows it
        // ── Immediate Context ──────────────────────────────────────────────
        auto Immediate = VulkanImmediateContext::Create(
            m_Device, m_TransferQueue, m_TransferFamily, m_GraphicsQueue, m_GraphicsFamily);
        if (!Immediate)
            return std::unexpected(Immediate.error().Append("VulkanImmediateContext creation failed"));
        m_ImmediateContext = std::move(*Immediate);

        // ── VulkanSwapchain ─────────────────────────────────────────────────────
        auto Swapchain = VulkanSwapchain::Create(m_Device, m_PhysicalDevice, m_Surface, *m_SurfaceProvider);
        if (!Swapchain)
            return std::unexpected(Swapchain.error());
        m_Swapchain = std::move(*Swapchain);

        // ── Timeline Semaphore ───────────────────────────────────────────
        auto Semaphore = VulkanTimelineSemaphore::Create(m_Device);
        if (!Semaphore)
            return std::unexpected(Semaphore.error());
        m_Timeline = std::move(*Semaphore);

        // Resource factory contexts borrow RenderDevice-owned Vulkan services.
        m_ResourceContext.emplace(
            m_Device, m_DebugUtils, m_Allocator, m_ImmediateContext, m_GraphicsFamily, m_TransferFamily);

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

        constexpr Uint64 TransientShaderStorageArenaCapacity = 4ULL * 1024ULL * 1024ULL;
        auto             TransientShaderStorageArena         = VulkanTransientShaderStorageArena::Create(
            "Renderer/Transient/ShaderStorageArena",
            TransientShaderStorageArenaCapacity,
            *m_ResourceContext,
            m_FramesInFlight);
        if (!TransientShaderStorageArena) {
            return std::unexpected(
                TransientShaderStorageArena.error().Append("VulkanTransientShaderStorageArena creation failed"));
        }
        m_TransientShaderStorageArena = std::move(*TransientShaderStorageArena);

        // ── Pre-register swapchain images in the committed state map ─────────
        RegisterSwapchainImages();

        // ── Global descriptor manager ─────────────────────────────────────
        {
            auto Heap = VulkanDescriptorManager::Create(m_Device, m_FramesInFlight);
            if (!Heap)
                return std::unexpected(Heap.error().Append("VulkanDescriptorManager creation failed"));
            m_DescriptorManager = std::make_unique<VulkanDescriptorManager>(std::move(*Heap));
        }

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

    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> {
        // CPU-GPU sync: wait for the timeline semaphore to reach the value
        // from N frames ago (when this slot was last signalled).
        uint64_t WaitValue = m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue;
        if (auto R = m_Timeline.Wait(WaitValue); !R)
            return std::unexpected(R.error().Append("BeginFrame: timeline wait failed"));

        // GPU done with this frame slot — safe to free scratch secondaries.
        m_FrameContext[m_CurrentFrame].ScratchSecondaries.clear();
        if (m_DescriptorManager)
            m_DescriptorManager->BeginFrame(m_CurrentFrame);

        // Free any GPU resources whose transfer operations have completed.
        RetireInFlightSubmissions();

        auto& PresentCompleteSema = m_FrameContext[m_CurrentFrame].PresentComplete;
        auto  AcquireRes          = m_Swapchain.AcquireNextImage(PresentCompleteSema);
        while (AcquireRes == vk::Result::eErrorOutOfDateKHR || AcquireRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(
                    R.error().Append("VulkanSwapchain recreation failed after AcquireNextImage error"));

            RegisterSwapchainImages();

            auto NewSema = m_Device.createSemaphore({});
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

        return {};
    }

    [[nodiscard]] auto EndFrame() -> std::expected<void, ErrorMessage> {
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        // ── End primary ─────────────────────────────────────────────────
        if (auto R = Primary.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("EndFrame: primary CB end failed: {}", vk::to_string(R))));

        // ── Submit ──────────────────────────────────────────────────────────
        vk::CommandBufferSubmitInfo PrimarySubmitInfo{
            .commandBuffer = *Primary,
        };

        vk::SemaphoreSubmitInfo PresentCompleteSema{
            .semaphore = *m_FrameContext[m_CurrentFrame].PresentComplete,
            .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
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

        if (auto R = m_GraphicsQueue.submit2({SubmitInfo2}); R != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format("Queue submit failed: {}", vk::to_string(R))));
        }

        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = TimelineSignalSema.value;

        // ── Present ────────────────────────────────────────────────────────
        auto PresentRes = m_Swapchain.Present(m_GraphicsQueue);
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(R.error().Append("VulkanSwapchain recreation failed after Present error"));
            RegisterSwapchainImages();
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
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

    [[nodiscard]] auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        auto Result = VulkanGraphicsPipeline::Create(*m_ResourceContext, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto Resource = RHIRef<RHIGraphicsPipeline>::Create();
        UPtr<RHIGraphicsPipeline> Payload = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override {
        auto Result = VulkanRayTracingPipeline::Create(*m_ResourceContext, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto Resource = RHIRef<RHIRayTracingPipeline>::Create();
        UPtr<RHIRayTracingPipeline> Payload = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(
        StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return EnqueueResourceCreation<RHIBottomLevelAccelerationStructure>(
            [this, Name = String(Name),
             Desc](RHIRef<RHIBottomLevelAccelerationStructure>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Result = VulkanBottomLevelAccelerationStructure::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                return Resource.Publish(std::move(*Result), RHIRefState::Ready);
            });
    }

    [[nodiscard]] auto CreateTopLevelAccelerationStructure(
        StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return EnqueueResourceCreation<RHITopLevelAccelerationStructure>(
            [this, Name = String(Name),
             Desc](RHIRef<RHITopLevelAccelerationStructure>& Resource) mutable -> std::expected<void, ErrorMessage> {
                auto Result = VulkanTopLevelAccelerationStructure::Create(*m_ResourceContext, Name, Desc);
                if (!Result) {
                    Resource.MarkFailed(Result.error());
                    return std::unexpected(Result.error());
                }
                return Resource.Publish(std::move(*Result), RHIRefState::Ready);
            });
    }

    auto TickBackendCompletions() -> void override {
        m_ImmediateContext.Tick();
    }

    auto WaitIdle() -> void override {
        if (*m_Device)
            (void)m_Device.waitIdle();
    }

    auto Shutdown() -> void override {
        if (std::exchange(m_NeedsShutdown, false)) {
            WaitIdle();
            ImGui_ImplVulkan_Shutdown();
            auto ImmediateDrain = m_ImmediateContext.Drain();
            if (!ImmediateDrain)
                LogError("{}", ImmediateDrain.error().ToString());
            WaitIdle();
            m_InFlightSubmissions.clear();
            GetDeletionQueue().Drain();
        }

        m_ResourceContext.reset();

        // Destroy VMA-backed buffers before vmaDestroyAllocator.
        m_TransientShaderStorageArena = {};
        m_TransientUniformArena       = {};
        m_FrameContext.clear();
        m_DescriptorManager.reset();
        if (m_Allocator) {
            vmaDestroyAllocator(m_Allocator);
            m_Allocator = nullptr;
        }
    }

  private:
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
        InitInfo.Instance            = static_cast<VkInstance>(*m_Instance);
        InitInfo.PhysicalDevice      = static_cast<VkPhysicalDevice>(*m_PhysicalDevice);
        InitInfo.Device              = static_cast<VkDevice>(*m_Device);
        InitInfo.QueueFamily         = m_GraphicsFamily;
        InitInfo.Queue               = static_cast<VkQueue>(*m_GraphicsQueue);
        InitInfo.DescriptorPool      = static_cast<VkDescriptorPool>(m_DescriptorManager->GetDescriptorPool());
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
                &m_Instance))
            return std::unexpected(ErrorMessage("ImGui_ImplVulkan_LoadFunctions failed"));
        if (!ImGui_ImplVulkan_Init(&InitInfo))
            return std::unexpected(ErrorMessage("ImGui_ImplVulkan_Init failed"));

        return {};
    }

    template <typename T, typename CreateFn>
    [[nodiscard]] auto EnqueueResourceCreation(RHIRef<T> Resource, CreateFn&& Create)
        -> std::expected<void, ErrorMessage> {
        auto TaskRef       = Resource;
        auto EnqueueResult = TaskGraph::Get().Enqueue(
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
    [[nodiscard]] auto CreateInstance(vk::raii::Context& Context, IVulkanSurfaceProvider& SurfaceProvider)
        -> std::expected<void, ErrorMessage> {
        vk::ApplicationInfo AppInfo{
            .pApplicationName   = "SoulEngine Application",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName        = "SoulEngine",
            .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion         = vk::ApiVersion14,
        };

        auto EnabledLayers = VulkanCapability::Get().ResolveInstanceLayers(Context);
        if (!EnabledLayers)
            return std::unexpected(EnabledLayers.error());
        const bool DebugUtils = ConfigManager::Get().GetConfig().RhiVulkan.DebugUtils.value_or(true);

        auto RequiredInstanceExtensions = SurfaceProvider.GetRequiredInstanceExtensions();
        if (!RequiredInstanceExtensions)
            return std::unexpected(
                RequiredInstanceExtensions.error().Append("Failed to query window-system Vulkan extensions"));
        if (DebugUtils)
            RequiredInstanceExtensions->emplace_back(vk::EXTDebugUtilsExtensionName);

        auto EnabledInstanceExts =
            VulkanCapability::Get().ResolveInstanceExtensions(Context, *RequiredInstanceExtensions);
        if (!EnabledInstanceExts.has_value())
            return std::unexpected(EnabledInstanceExts.error());

        for (auto* Ext : *EnabledInstanceExts)
            LogDebug("Enabled instance extension: {}", Ext);
        for (auto* Layer : *EnabledLayers)
            LogDebug("Enabled instance layer: {}", Layer);

        vk::InstanceCreateInfo InstCI{
            .pApplicationInfo        = &AppInfo,
            .enabledLayerCount       = static_cast<uint32_t>(EnabledLayers->size()),
            .ppEnabledLayerNames     = EnabledLayers->data(),
            .enabledExtensionCount   = static_cast<uint32_t>(EnabledInstanceExts->size()),
            .ppEnabledExtensionNames = EnabledInstanceExts->data(),
        };
        if (VulkanCapability::Get().IsInstanceExtensionEnabled(vk::KHRPortabilityEnumerationExtensionName))
            InstCI.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;

        if (DebugUtils) {
            auto DebugMessengerCI = CreateDebugMessengerCI();
            vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> InstanceChain{
                InstCI,
                DebugMessengerCI,
            };
            auto InstanceResult = Context.createInstance(InstanceChain.get<vk::InstanceCreateInfo>());
            if (InstanceResult.result != vk::Result::eSuccess) {
                return std::unexpected(
                    ErrorMessage(Format("Failed to create Vulkan instance: {}", vk::to_string(InstanceResult.result))));
            }
            m_Instance = std::move(InstanceResult.value);
        } else {
            auto InstanceResult = Context.createInstance(InstCI);
            if (InstanceResult.result != vk::Result::eSuccess) {
                return std::unexpected(
                    ErrorMessage(Format("Failed to create Vulkan instance: {}", vk::to_string(InstanceResult.result))));
            }
            m_Instance = std::move(InstanceResult.value);
        }

        if (DebugUtils) {
            auto DebugMessenger = m_Instance.createDebugUtilsMessengerEXT(CreateDebugMessengerCI(), nullptr);
            if (DebugMessenger.result != vk::Result::eSuccess) {
                return std::unexpected(ErrorMessage(
                    Format("Failed to create Vulkan debug messenger: {}", vk::to_string(DebugMessenger.result))));
            }
            m_DebugMessenger = std::move(DebugMessenger.value);
        }
        return {};
    }

    [[nodiscard]] auto PickPhysicalDevice() -> std::expected<void, ErrorMessage> {
        auto DevicesResult = m_Instance.enumeratePhysicalDevices();
        if (DevicesResult.result != vk::Result::eSuccess || DevicesResult.value.empty())
            return std::unexpected(ErrorMessage("No Vulkan-capable physical devices found"));

        vk::StructureChain<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties> PropsChain;

        for (std::size_t i = 0; i < DevicesResult.value.size(); ++i) {
            auto PD = DevicesResult.value[i];
            PD.getProperties2(&PropsChain.get<vk::PhysicalDeviceProperties2>());
            auto& DevProps = PropsChain.get<vk::PhysicalDeviceProperties2>().properties;
            auto& Driver   = PropsChain.get<vk::PhysicalDeviceDriverProperties>();
            LogDebug("Physical device [{}]: {} (driver: {}, ICD: {}, API version {}.{}.{})",
                     i,
                     static_cast<const char*>(DevProps.deviceName),
                     static_cast<const char*>(Driver.driverName),
                     vk::to_string(Driver.driverID),
                     VK_API_VERSION_MAJOR(DevProps.apiVersion),
                     VK_API_VERSION_MINOR(DevProps.apiVersion),
                     VK_API_VERSION_PATCH(DevProps.apiVersion));
        }

        m_PhysicalDevice = std::move(DevicesResult.value[0]);

        {
            m_PhysicalDevice.getProperties2(&PropsChain.get<vk::PhysicalDeviceProperties2>());
            auto& DevProps = PropsChain.get<vk::PhysicalDeviceProperties2>().properties;
            auto& Driver   = PropsChain.get<vk::PhysicalDeviceDriverProperties>();
            LogInfo("Selected GPU: {} (driver: {}, ICD: {}, API version {}.{}.{})",
                    static_cast<const char*>(DevProps.deviceName),
                    static_cast<const char*>(Driver.driverName),
                    vk::to_string(Driver.driverID),
                    VK_API_VERSION_MAJOR(DevProps.apiVersion),
                    VK_API_VERSION_MINOR(DevProps.apiVersion),
                    VK_API_VERSION_PATCH(DevProps.apiVersion));
        }

        auto QueueProps = m_PhysicalDevice.getQueueFamilyProperties2();

        if (auto Res = ResolveQueueFamilies(QueueProps); !Res.has_value())
            return std::unexpected(Res.error());

        return {};
    }

    [[nodiscard]] auto CreateLogicalDevice() -> std::expected<void, ErrorMessage> {
        std::vector<uint32_t> UniqueFamilies;

        auto AddUnique = [&](uint32_t Family) {
            if (std::ranges::find(UniqueFamilies, Family) == UniqueFamilies.end())
                UniqueFamilies.push_back(Family);
        };

        AddUnique(m_GraphicsFamily);
        AddUnique(m_ComputeFamily);
        AddUnique(m_TransferFamily);

        float                                  QueuePriority = 1.0f;
        std::vector<vk::DeviceQueueCreateInfo> QueueCIs;
        for (uint32_t Family : UniqueFamilies) {
            QueueCIs.push_back(vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = Family,
                .queueCount       = 1,
                .pQueuePriorities = &QueuePriority,
            });
        }

        auto EnabledDeviceExts = VulkanCapability::Get().ResolveDeviceExtensionsAndFeatures(m_PhysicalDevice);
        if (!EnabledDeviceExts.has_value())
            return std::unexpected(EnabledDeviceExts.error());

        auto& [DevExts, FeatsChain] = *EnabledDeviceExts;

        for (auto* Ext : DevExts)
            LogDebug("Enabled device extension: {}", Ext);

        vk::DeviceCreateInfo DevCI{
            .pNext                   = &FeatsChain.get<vk::PhysicalDeviceFeatures2>(),
            .queueCreateInfoCount    = static_cast<uint32_t>(QueueCIs.size()),
            .pQueueCreateInfos       = QueueCIs.data(),
            .enabledExtensionCount   = static_cast<uint32_t>(DevExts.size()),
            .ppEnabledExtensionNames = DevExts.data(),
            .pEnabledFeatures        = nullptr,
        };

        auto DevResult = m_PhysicalDevice.createDevice(DevCI);
        if (DevResult.result != vk::Result::eSuccess) {
            return std::unexpected(
                ErrorMessage(Format("Failed to create logical device: {}", vk::to_string(DevResult.result))));
        }
        m_Device = std::move(DevResult.value);

        // ── Verify required features ────────────────────────────────────
        const auto& V13 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan13Features>();
        if (!V13.synchronization2)
            return std::unexpected(ErrorMessage("synchronization2 feature not supported by device"));
        if (!V13.dynamicRendering)
            return std::unexpected(ErrorMessage("dynamicRendering feature not supported by device"));

        m_GraphicsQueue = m_Device.getQueue(m_GraphicsFamily, 0);
        m_ComputeQueue  = m_Device.getQueue(m_ComputeFamily, 0);
        m_TransferQueue = m_Device.getQueue(m_TransferFamily, 0);

        VulkanCapability::Get().ResolveDeviceProperties(m_PhysicalDevice);
        if (VulkanCapability::Get().IsRayTracingAvailable())
            LogInfo("Hardware ray tracing capability is available on the selected GPU");
        else
            LogInfo("Hardware ray tracing capability is unavailable");

        return {};
    }

    [[nodiscard]] auto CreateVMA(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
        const auto&        CtxDispatcher  = Context.getDispatcher();
        const auto&        InstDispatcher = m_Instance.getDispatcher();
        VmaVulkanFunctions VmaVF{
            .vkGetInstanceProcAddr = CtxDispatcher->vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr   = InstDispatcher->vkGetDeviceProcAddr,
        };

        VmaAllocatorCreateInfo VmaInfo{
            .physicalDevice   = static_cast<VkPhysicalDevice>(*m_PhysicalDevice),
            .device           = static_cast<VkDevice>(*m_Device),
            .pVulkanFunctions = &VmaVF,
            .instance         = static_cast<VkInstance>(*m_Instance),
            .vulkanApiVersion = VK_API_VERSION_1_4,
        };
        if (VulkanCapability::Get().IsDeviceExtensionEnabled(vk::EXTMemoryBudgetExtensionName))
            VmaInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        if (VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress)
            VmaInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        VkResult Result = vmaCreateAllocator(&VmaInfo, &m_Allocator);
        if (Result != VK_SUCCESS)
            return std::unexpected(ErrorMessage(
                Format("Failed to create VMA allocator: {}", vk::to_string(static_cast<vk::Result>(Result)))));
        return {};
    }

    [[nodiscard]] auto ResolveQueueFamilies(std::span<const vk::QueueFamilyProperties2> QueueProps)
        -> std::expected<void, ErrorMessage> {
        for (std::size_t i = 0; i < QueueProps.size(); ++i) {
            LogDebug("Queue family [{}]: count={}, flags={}",
                     i,
                     QueueProps[i].queueFamilyProperties.queueCount,
                     vk::to_string(QueueProps[i].queueFamilyProperties.queueFlags));
        }

        // Graphics + Present
        {
            for (std::size_t i = 0; i < QueueProps.size(); ++i) {
                if ((QueueProps[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) !=
                    vk::QueueFlags{}) {
                    auto [Res, Supported] = m_PhysicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), m_Surface);
                    if (Res == vk::Result::eSuccess && Supported) {
                        m_GraphicsFamily = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }
        }

        if (m_GraphicsFamily == vk::QueueFamilyIgnored)
            return std::unexpected(ErrorMessage("No queue family supports both graphics and presentation"));

        // Compute
        auto ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
        });
        if (ComputeIt == QueueProps.end()) {
            ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{};
            });
        }
        if (ComputeIt == QueueProps.end())
            return std::unexpected(ErrorMessage("No compute-capable queue family found"));
        m_ComputeFamily = static_cast<uint32_t>(std::distance(QueueProps.begin(), ComputeIt));

        // Transfer
        auto TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) == vk::QueueFlags{};
        });
        if (TransferIt == QueueProps.end()) {
            TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                       (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
            });
        }
        if (TransferIt == QueueProps.end()) {
            TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{};
            });
        }
        if (TransferIt == QueueProps.end())
            return std::unexpected(ErrorMessage("No transfer-capable queue family found"));
        m_TransferFamily = static_cast<uint32_t>(std::distance(QueueProps.begin(), TransferIt));

        LogInfo("Queue families resolved: graphics={}, compute={}, transfer={}",
                m_GraphicsFamily,
                m_ComputeFamily,
                m_TransferFamily);

        return {};
    }

    auto RegisterSwapchainImages() -> void {
        for (uint32_t i = 0; i < m_Swapchain.GetImageCount(); ++i) {
            auto Image                    = m_Swapchain.GetImage(i);
            m_CommittedImageStates[Image] = VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eNone,
                .access = vk::AccessFlagBits2::eNone,
                .layout = vk::ImageLayout::eUndefined,
            };
        }
    }

    [[nodiscard]] auto ValidateCommandList(const RHICommandList& CmdList) const -> std::expected<void, ErrorMessage> {
        if (CmdList.Scopes.empty() && !CmdList.PresentSourceRef.TryGet() && !CmdList.ImGuiPresentationOverlay)
            return {};

        if (!CmdList.PresentSourceRef.TryGet())
            return std::unexpected(ErrorMessage("Execute: command list with scopes must specify PresentSource"));

        const auto ValidateCommands = [](std::span<const RHICommand> Commands,
                                         bool bRenderingScope) -> std::expected<void, ErrorMessage> {
            RHIPipeline* BoundPipeline = nullptr;
            for (const auto& Cmd : Commands) {
                const auto ValidateCommand =
                    [&BoundPipeline, bRenderingScope](const auto& TypedCmd) -> std::expected<void, ErrorMessage> {
                    using CommandType = std::decay_t<decltype(TypedCmd)>;

                    if constexpr (std::is_same_v<CommandType, RHISetGraphicsPipelineCmd>) {
                        if (!bRenderingScope)
                            return std::unexpected(
                                ErrorMessage("Execute: graphics pipelines require a rendering scope"));
                        auto* Pipeline = TypedCmd.PipelineRef.TryGet();
                        if (!Pipeline)
                            return std::unexpected(
                                ErrorMessage("Execute: SetGraphicsPipeline is missing graphics pipeline"));
                        BoundPipeline = Pipeline;
                    } else if constexpr (std::is_same_v<CommandType, RHISetRayTracingPipelineCmd>) {
                        if (bRenderingScope)
                            return std::unexpected(
                                ErrorMessage("Execute: ray-tracing pipelines require a non-rendering scope"));
                        auto* Pipeline = TypedCmd.PipelineRef.TryGet();
                        if (!Pipeline)
                            return std::unexpected(
                                ErrorMessage("Execute: SetRayTracingPipeline is missing ray-tracing pipeline"));
                        BoundPipeline = Pipeline;
                    } else if constexpr (std::is_same_v<CommandType, RHIPushConstantsCmd>) {
                        RHIPipeline* Pipeline = TypedCmd.PipelineRef.Graphics.TryGet();
                        if (!Pipeline)
                            Pipeline = TypedCmd.PipelineRef.RayTracing.TryGet();
                        if (!Pipeline)
                            return std::unexpected(ErrorMessage("Execute: PushConstants is missing pipeline"));
                        if (BoundPipeline != Pipeline)
                            return std::unexpected(ErrorMessage(
                                "Execute: PushConstants pipeline does not match the currently bound pipeline"));
                        if (TypedCmd.Data.empty())
                            return std::unexpected(ErrorMessage("Execute: PushConstants data is empty"));
                    } else if constexpr (std::is_same_v<CommandType, RHIBindShaderParametersCmd>) {
                        RHIPipeline* Pipeline = TypedCmd.PipelineRef.Graphics.TryGet();
                        if (!Pipeline)
                            Pipeline = TypedCmd.PipelineRef.RayTracing.TryGet();
                        if (!Pipeline)
                            return std::unexpected(ErrorMessage("Execute: BindShaderParameters is missing pipeline"));
                        if (BoundPipeline != Pipeline) {
                            return std::unexpected(ErrorMessage(
                                "Execute: BindShaderParameters pipeline does not match the currently bound pipeline"));
                        }
                        if (TypedCmd.Parameters.GetSets().empty())
                            return std::unexpected(ErrorMessage("Execute: BindShaderParameters has no parameter sets"));
                    } else if constexpr (std::is_same_v<CommandType, RHIDrawIndexedCmd>) {
                        if (!bRenderingScope)
                            return std::unexpected(ErrorMessage("Execute: draw commands require a rendering scope"));
                        auto* Pipeline = TypedCmd.PipelineRef.TryGet();
                        if (!Pipeline)
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing graphics pipeline"));
                        if (BoundPipeline != Pipeline) {
                            return std::unexpected(ErrorMessage(
                                "Execute: indexed draw pipeline does not match the currently bound graphics pipeline"));
                        }
                        if (!TypedCmd.VertexBufferRefs[0].TryGet())
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing vertex buffer"));
                        if (!TypedCmd.IndexBufferRef.TryGet())
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing index buffer"));
                    } else if constexpr (std::is_same_v<CommandType, RHIDrawCmd>) {
                        if (!bRenderingScope)
                            return std::unexpected(ErrorMessage("Execute: draw commands require a rendering scope"));
                        auto* Pipeline = TypedCmd.PipelineRef.TryGet();
                        if (!Pipeline)
                            return std::unexpected(ErrorMessage("Execute: draw is missing graphics pipeline"));
                        if (BoundPipeline != Pipeline) {
                            return std::unexpected(ErrorMessage(
                                "Execute: draw pipeline does not match the currently bound graphics pipeline"));
                        }
                    } else if constexpr (std::is_same_v<CommandType, RHIWriteTransientConstantBufferCmd>) {
                        if (!TypedCmd.Buffer.IsValid())
                            return std::unexpected(
                                ErrorMessage("Execute: transient constant buffer write has an invalid buffer"));
                        if (TypedCmd.Data.size() != TypedCmd.Buffer.GetSize()) {
                            return std::unexpected(ErrorMessage(
                                "Execute: transient constant buffer write size does not match buffer size"));
                        }
                    } else if constexpr (std::is_same_v<CommandType, RHIWriteTransientShaderStorageBufferCmd>) {
                        if (!TypedCmd.Buffer.IsValid()) {
                            return std::unexpected(
                                ErrorMessage("Execute: transient shader storage buffer write has an invalid buffer"));
                        }
                        if (TypedCmd.Data.size() != TypedCmd.Buffer.GetSize()) {
                            return std::unexpected(ErrorMessage(
                                "Execute: transient shader storage buffer write size does not match buffer size"));
                        }
                    } else if constexpr (std::is_same_v<CommandType,
                                                        RHIBuildOrUpdateTopLevelAccelerationStructureCmd>) {
                        if (bRenderingScope) {
                            return std::unexpected(
                                ErrorMessage("Execute: acceleration-structure builds require a non-rendering scope"));
                        }
                        if (!TypedCmd.TargetRef.TryGet())
                            return std::unexpected(ErrorMessage("Execute: TLAS build is missing its target"));
                    } else if constexpr (std::is_same_v<CommandType, RHITraceRaysCmd>) {
                        if (bRenderingScope)
                            return std::unexpected(ErrorMessage("Execute: TraceRays requires a non-rendering scope"));
                        auto* Pipeline = TypedCmd.PipelineRef.TryGet();
                        if (!Pipeline)
                            return std::unexpected(ErrorMessage("Execute: TraceRays is missing ray-tracing pipeline"));
                        if (BoundPipeline != Pipeline) {
                            return std::unexpected(ErrorMessage(
                                "Execute: TraceRays pipeline does not match the currently bound ray-tracing pipeline"));
                        }
                        if (TypedCmd.Width == 0 || TypedCmd.Height == 0 || TypedCmd.Depth == 0)
                            return std::unexpected(ErrorMessage("Execute: TraceRays dimensions must be non-zero"));
                    } else if constexpr (std::is_same_v<CommandType, RHISetViewportCmd> ||
                                         std::is_same_v<CommandType, RHISetFullViewportCmd> ||
                                         std::is_same_v<CommandType, RHISetScissorCmd> ||
                                         std::is_same_v<CommandType, RHISetFullScissorRectCmd>) {
                        if (!bRenderingScope)
                            return std::unexpected(
                                ErrorMessage("Execute: viewport and scissor commands require a rendering scope"));
                    }

                    return {};
                };

                if (auto R = std::visit(ValidateCommand, Cmd); !R)
                    return std::unexpected(R.error());
            }
            return {};
        };

        for (const auto& Scope : CmdList.Scopes) {
            const auto ValidateScope =
                [&ValidateCommands](const auto& TypedScope) -> std::expected<void, ErrorMessage> {
                using ScopeType = std::decay_t<decltype(TypedScope)>;
                if constexpr (std::is_same_v<ScopeType, RHIPass>) {
                    if (TypedScope.Desc.ColorAttachments.empty()) {
                        return std::unexpected(
                            ErrorMessage("Execute: pass color attachment must be an explicit render target"));
                    }
                    return ValidateCommands(TypedScope.Commands, true);
                } else {
                    return ValidateCommands(TypedScope.Commands, false);
                }
            };
            if (auto R = std::visit(ValidateScope, Scope); !R)
                return std::unexpected(R.error());
        }

        return {};
    }

    auto RecordPresentBlit(vk::raii::CommandBuffer& Buf, RHIRenderTarget* Source, bool TransitionForPresent) -> void {
        auto& SrcRT = static_cast<const VulkanRenderTarget&>(*Source);

        const auto SrcImage = SrcRT.GetVkImage();
        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());

        VulkanTransitionImage(Buf,
                              m_CommittedImageStates,
                              SrcImage,
                              vk::PipelineStageFlagBits2::eTransfer,
                              vk::AccessFlagBits2::eTransferRead,
                              vk::ImageLayout::eTransferSrcOptimal,
                              false,
                              ToVkImageAspect(SrcRT.GetFormat()));
        // The acquire semaphore is waited at Transfer. Make the first
        // swapchain barrier source stage participate in that wait so sync
        // validation sees the acquire read ordered before our transfer write.
        auto& DstState  = m_CommittedImageStates[DstImage];
        DstState.stage  = vk::PipelineStageFlagBits2::eTransfer;
        DstState.access = vk::AccessFlagBits2::eNone;

        VulkanTransitionImage(Buf,
                              m_CommittedImageStates,
                              DstImage,
                              vk::PipelineStageFlagBits2::eTransfer,
                              vk::AccessFlagBits2::eTransferWrite,
                              vk::ImageLayout::eTransferDstOptimal,
                              true);

        const auto    DstExtent = m_Swapchain.GetExtent();
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

        if (TransitionForPresent) {
            VulkanTransitionImage(Buf,
                                  m_CommittedImageStates,
                                  DstImage,
                                  vk::PipelineStageFlagBits2::eBottomOfPipe,
                                  vk::AccessFlagBits2::eNone,
                                  vk::ImageLayout::ePresentSrcKHR,
                                  false);
        }
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
        VulkanTransitionImage(Buf,
                              m_CommittedImageStates,
                              DstImage,
                              vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                              vk::AccessFlagBits2::eColorAttachmentWrite,
                              vk::ImageLayout::eColorAttachmentOptimal,
                              false);

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

        VulkanTransitionImage(Buf,
                              m_CommittedImageStates,
                              DstImage,
                              vk::PipelineStageFlagBits2::eBottomOfPipe,
                              vk::AccessFlagBits2::eNone,
                              vk::ImageLayout::ePresentSrcKHR,
                              false);
        return {};
    }

    // VulkanCommandVisitor lives in VKCommand.cppm — imported via :Command partition.

    // ═════════════════════════════════════════════════════════════════════════════
    // Execute — consume RHICommandList, record and submit
    // ═════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] auto Execute(RHICommandList&& CmdList) -> std::expected<void, ErrorMessage> override {
        if (auto R = ValidateCommandList(CmdList); !R)
            return std::unexpected(R.error());
        if (CmdList.Scopes.empty() && !CmdList.PresentSourceRef.TryGet() && !CmdList.ImGuiPresentationOverlay)
            return {};

        if (auto R = BeginFrame(); !R)
            return R;
        if (auto R = m_TransientUniformArena.BeginFrame(m_CurrentFrame); !R)
            return std::unexpected(R.error().Append("Execute: transient constant arena reset failed"));
        if (auto R = m_TransientShaderStorageArena.BeginFrame(m_CurrentFrame); !R)
            return std::unexpected(R.error().Append("Execute: transient shader storage arena reset failed"));
        const Uint64 FrameTokenValue = m_Timeline.NextValue();

        std::vector<vk::CommandBuffer> Secondaries;
        Secondaries.reserve(CmdList.Scopes.size());
        std::vector<std::pair<RHITransientConstantBuffer, VulkanTransientBufferSlice>> TransientConstantBuffers;
        std::vector<std::pair<RHITransientShaderStorageBuffer, VulkanTransientBufferSlice>>
                                           TransientShaderStorageBuffers;
        std::vector<std::function<void()>> RetiredPayloads;

        for (const auto& Scope : CmdList.Scopes) {
            // Allocate one secondary for this ordered rendering or non-rendering scope.
            vk::CommandBufferAllocateInfo Alloc{
                .commandPool        = *m_FrameContext[m_CurrentFrame].SubPool,
                .level              = vk::CommandBufferLevel::eSecondary,
                .commandBufferCount = 1,
            };
            auto AllocResult = m_Device.allocateCommandBuffers(Alloc);
            if (AllocResult.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Execute: failed to allocate secondary CB"));
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
                auto                 ImageStateCopy = m_CommittedImageStates;
                VulkanCommandVisitor Visitor{
                    .Buf                           = SecBuf,
                    .LocalStates                   = ImageStateCopy,
                    .Descriptors                   = m_DescriptorManager.get(),
                    .TransientUniformArena         = &m_TransientUniformArena,
                    .TransientShaderStorageArena   = &m_TransientShaderStorageArena,
                    .TransientConstantBuffers      = &TransientConstantBuffers,
                    .TransientShaderStorageBuffers = &TransientShaderStorageBuffers,
                    .RetiredPayloads               = &RetiredPayloads,
                    .FrameIndex                    = m_CurrentFrame,
                };
                const auto IsTransientUpload = [](const RHICommand& Cmd) -> bool {
                    return std::holds_alternative<RHIWriteTransientConstantBufferCmd>(Cmd) ||
                           std::holds_alternative<RHIWriteTransientShaderStorageBufferCmd>(Cmd) ||
                           std::holds_alternative<RHIWriteRayTracingGeometryDataCmd>(Cmd) ||
                           std::holds_alternative<RHIWriteRasterGeometryDataCmd>(Cmd);
                };
                const auto RecordCommand = [&Visitor](const RHICommand& Cmd) -> std::expected<void, ErrorMessage> {
                    std::visit(Visitor, Cmd);
                    if (Visitor.Error)
                        return std::unexpected(*Visitor.Error);
                    return {};
                };
                const auto RecordScope = [&Visitor, &IsTransientUpload, &RecordCommand](
                                             const auto& TypedScope) -> std::expected<void, ErrorMessage> {
                    using ScopeType = std::decay_t<decltype(TypedScope)>;
                    if constexpr (std::is_same_v<ScopeType, RHIPass>) {
                        // Host writes require HOST pipeline stages, which Vulkan forbids inside dynamic rendering.
                        // Resolve every transient upload before opening the rendering scope.
                        for (const auto& Cmd : TypedScope.Commands) {
                            if (!IsTransientUpload(Cmd))
                                continue;
                            if (auto R = RecordCommand(Cmd); !R)
                                return R;
                        }
                        // Storage-image layout transitions are illegal inside dynamic rendering.
                        // Pre-record pipeline and descriptor bindings so their image barriers land before
                        // BeginRendering.
                        for (const auto& Cmd : TypedScope.Commands) {
                            if (IsTransientUpload(Cmd) || (!std::holds_alternative<RHISetGraphicsPipelineCmd>(Cmd) &&
                                                           !std::holds_alternative<RHIBindShaderParametersCmd>(Cmd)))
                                continue;
                            if (auto R = RecordCommand(Cmd); !R)
                                return R;
                        }
                        Visitor.BeginPass(TypedScope.Desc);
                        if (Visitor.Error)
                            return std::unexpected(*Visitor.Error);
                        for (const auto& Cmd : TypedScope.Commands) {
                            if (IsTransientUpload(Cmd))
                                continue;
                            if (auto R = RecordCommand(Cmd); !R)
                                return R;
                        }
                        Visitor.EndPass();
                        return {};
                    }
                    for (const auto& Cmd : TypedScope.Commands) {
                        if (auto R = RecordCommand(Cmd); !R)
                            return R;
                    }
                    return {};
                };
                if (auto R = std::visit(RecordScope, Scope); !R)
                    return std::unexpected(R.error());
                m_CommittedImageStates = std::move(ImageStateCopy);
            }
            if (auto R = SecBuf.end(); R != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage(Format("Execute: secondary CB end failed: {}", vk::to_string(R))));
            Secondaries.push_back(static_cast<vk::CommandBuffer>(*SecBuf));
        }

        // Primary CB was already prepared in BeginFrame — just execute secondaries
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        if (!Secondaries.empty())
            Primary.executeCommands(Secondaries);
        RecordPresentBlit(Primary, CmdList.PresentSourceRef.TryGet(), !CmdList.ImGuiPresentationOverlay.has_value());
        if (CmdList.ImGuiPresentationOverlay) {
            if (auto R = RecordImGuiPresentationOverlay(Primary, *CmdList.ImGuiPresentationOverlay); !R)
                return std::unexpected(R.error());
        }
        if (auto R = Primary.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Execute: primary end failed"));

        vk::CommandBufferSubmitInfo PrimarySubmitInfo{.commandBuffer = *Primary};
        vk::SemaphoreSubmitInfo     PresentCompleteSema{
            .semaphore = *m_FrameContext[m_CurrentFrame].PresentComplete,
            .stageMask = vk::PipelineStageFlagBits2::eTransfer,
        };
        vk::SemaphoreSubmitInfo RenderingCompleteSema{
            .semaphore = m_Swapchain.GetCurrentRenderCompleteSemaphore(),
            .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
        };
        vk::SemaphoreSubmitInfo TimelineSignalSema{
            .semaphore = m_Timeline.Get(),
            .value     = FrameTokenValue,
            .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
        };
        vk::SemaphoreSubmitInfo SignalSemas[]{RenderingCompleteSema, TimelineSignalSema};

        vk::SubmitInfo2 SubmitInfo2{
            .waitSemaphoreInfoCount   = 1,
            .pWaitSemaphoreInfos      = &PresentCompleteSema,
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &PrimarySubmitInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos    = SignalSemas,
        };
        if (auto R = m_GraphicsQueue.submit2({SubmitInfo2}); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("Queue submit failed: {}", vk::to_string(R))));
        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = FrameTokenValue;
        m_InFlightSubmissions.push_back(VulkanInFlightSubmission{
            .CompletionTimelineValue = FrameTokenValue,
            .CommandList             = std::move(CmdList),
            .RetiredPayloads         = std::move(RetiredPayloads),
        });

        auto PresentRes = m_Swapchain.Present(m_GraphicsQueue);
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            if (auto R = m_Swapchain.Recreate(); !R)
                return std::unexpected(R.error().Append("VulkanSwapchain recreation failed after Present error"));
            RegisterSwapchainImages();
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
        return {};
    }

    // ── RAII resources ─────────────────────────────────────────────────────

    UPtr<IVulkanSurfaceProvider>     m_SurfaceProvider = nullptr;
    vk::raii::Instance               m_Instance        = nullptr;
    vk::raii::DebugUtilsMessengerEXT m_DebugMessenger  = nullptr;
    vk::raii::SurfaceKHR             m_Surface         = nullptr;
    vk::raii::PhysicalDevice         m_PhysicalDevice  = nullptr;
    vk::raii::Device                 m_Device          = nullptr;
    VulkanDebugUtils                 m_DebugUtils;

    uint32_t m_GraphicsFamily = vk::QueueFamilyIgnored;
    uint32_t m_ComputeFamily  = vk::QueueFamilyIgnored;
    uint32_t m_TransferFamily = vk::QueueFamilyIgnored;

    vk::raii::Queue m_GraphicsQueue = nullptr;
    vk::raii::Queue m_ComputeQueue  = nullptr;
    vk::raii::Queue m_TransferQueue = nullptr;

    VulkanImmediateContext m_ImmediateContext;

    uint32_t m_FramesInFlight = 2;
    uint32_t m_CurrentFrame   = 0;

    // ── VulkanSwapchain & sync ──────────────────────────────────────────────────

    VulkanSwapchain         m_Swapchain;
    VmaAllocator            m_Allocator = nullptr;
    VulkanTimelineSemaphore m_Timeline;

    struct VulkanInFlightSubmission {
        Uint64                             CompletionTimelineValue = 0;
        RHICommandList                     CommandList             = {};
        std::vector<std::function<void()>> RetiredPayloads         = {};
    };
    std::deque<VulkanInFlightSubmission> m_InFlightSubmissions = {};

    std::optional<VulkanResourceContext> m_ResourceContext = std::nullopt;

    std::vector<VulkanFrameContext>   m_FrameContext;
    VulkanTransientUniformArena       m_TransientUniformArena       = {};
    VulkanTransientShaderStorageArena m_TransientShaderStorageArena = {};

    // ── Global descriptor manager ─────────────────────────────────────────
    UPtr<VulkanDescriptorManager> m_DescriptorManager = nullptr;
    bool                          m_NeedsShutdown     = false;

    // ── Barrier state tracking ───────────────────────────────────────────

    std::unordered_map<vk::Image, VulkanImageState> m_CommittedImageStates;
};

} // namespace SoulEngine
