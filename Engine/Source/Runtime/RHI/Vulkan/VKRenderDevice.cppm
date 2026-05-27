module;

// For VMA loading procedure, see:
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html
// Because we use `DynamicLoader` from vulkan headers, we define:
// dynamically fetching pointers using `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module Vulkan:RenderDevice;

import RHI;
import vulkan;
import std;

import :Swapchain;
import :CommandList;
import :Types;
import :Semaphore;
import :Buffer;
import :Capability;
import :ImmediateContext;
import :DeletionQueue;
import :Descriptor;
import :Pipeline;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Per-frame-in-flight state: semaphores, timeline value, and the
/// command buffer for this frame slot — everything that cycles with
/// the frame index lives here.
///
/// Named FrameContext to reserve FrameData for the future game→render
/// thread transfer struct that will carry draw commands, view state, etc.
struct FrameContext {
    vk::raii::Semaphore     PresentComplete                 = nullptr;
    Uint64                  SubmissionCompleteTimelineValue = 0;
    vk::raii::CommandBuffer CommandBuffer                   = nullptr;
};

// ═════════════════════════════════════════════════════════════════════════════
// RenderDevice
// ═════════════════════════════════════════════════════════════════════════════

class RenderDevice final : public RHI::RenderDevice {
  public:
    RenderDevice() {}
    ~RenderDevice() {}

    [[nodiscard]] auto Init(GLFWwindow* Window) -> std::expected<void, ErrorMessage> override {
        // Off-screen rendering is not on the roadmap; every frame is presented
        // to a GLFW window, so a valid window handle is mandatory.
        if (!Window)
            return std::unexpected(ErrorMessage("Window is null — off-screen rendering is not supported"));

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

        if (auto Res = CreateInstance(Context); !Res.has_value())
            return std::unexpected(Res.error());

        // Surface must exist before PickPhysicalDevice so we can verify
        // surface presentation support via getSurfaceSupportKHR.
        if (auto Res = CreateSurface(Window); !Res.has_value())
            return std::unexpected(Res.error());

        if (auto Res = PickPhysicalDevice(); !Res.has_value())
            return std::unexpected(Res.error());

        if (auto Res = CreateLogicalDevice(); !Res.has_value())
            return std::unexpected(Res.error());

        // ── VMA ───────────────────────────────────────────────────────────
        if (auto Res = CreateVMA(Context); !Res.has_value())
            return std::unexpected(Res.error());

        // ── Deferred Deletion Queue ────────────────────────────────────────
        // Must be created before ImmediateContext — ImmediateContext borrows it
        auto DelQueue = DeferredDeletionQueue::Create(m_Device);
        if (!DelQueue)
            return std::unexpected(DelQueue.error().Append("DeferredDeletionQueue creation failed"));
        m_DeletionQueue = std::move(*DelQueue);

        // ── Immediate Context ──────────────────────────────────────────────
        auto ImmCtx = ImmediateContext::Create(m_Device, m_TransferQueue, m_TransferFamily, m_DeletionQueue);
        if (!ImmCtx)
            return std::unexpected(ImmCtx.error().Append("ImmediateContext creation failed"));
        m_ImmediateContext = std::move(*ImmCtx);

        // ── Swapchain ─────────────────────────────────────────────────────
        auto Swapchain = Swapchain::Create(m_Device, m_PhysicalDevice, m_Surface, Window);
        if (!Swapchain)
            return std::unexpected(Swapchain.error());
        m_Swapchain = std::move(*Swapchain);

        // ── Timeline Semaphore ───────────────────────────────────────────
        auto Semaphore = TimelineSemaphore::Create(m_Device);
        if (!Semaphore)
            return std::unexpected(Semaphore.error());
        m_Timeline = std::move(*Semaphore);

        // ── Main command pool + per-frame command buffers ──────────────
        // Single pool owned by RenderDevice.  One primary command buffer
        // per frame-in-flight, stored inside FrameContext alongside its
        // semaphores and timeline value.
        vk::CommandPoolCreateInfo PoolCI{
            .flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = m_GraphicsFamily,
        };
        auto PoolRes = m_Device.createCommandPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create main command pool"));
        m_MainCommandPool = std::move(PoolRes.value);

        vk::CommandBufferAllocateInfo AllocInfo{
            .commandPool        = *m_MainCommandPool,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = m_FramesInFlight,
        };
        auto BufResult = m_Device.allocateCommandBuffers(AllocInfo);
        if (BufResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to allocate main command buffers"));

        // ── Present-complete semaphores + FrameContext ──────────────────
        m_FrameContext.clear();
        m_FrameContext.reserve(m_FramesInFlight);
        for (uint32_t i = 0; i < m_FramesInFlight; ++i) {
            auto SemaRes = m_Device.createSemaphore({});
            if (SemaRes.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create present-complete semaphore"));
            m_FrameContext.push_back({
                .PresentComplete                 = std::move(SemaRes.value),
                .SubmissionCompleteTimelineValue = 0,
                .CommandBuffer                   = std::move(BufResult.value[i]),
            });
        }

        // ── Pre-register swapchain images in the committed state map ─────────
        // Note: Initial stage = COLOR_ATTACHMENT_OUTPUT so the first-use barrier's
        //       srcStageMask matches the semaphore wait stage from EndFrame's submit,
        //       forming a correct execution dependency chain and avoiding
        //       WRITE_AFTER_READ hazards with vkAcquireNextImageKHR.
        RegisterSwapchainImages();

        // ── Global descriptor manager (descriptor sets + shared pipeline layout) ──
        {
            auto Heap = DescriptorManager::Create(m_Device);
            if (!Heap)
                return std::unexpected(Heap.error().Append("DescriptorManager creation failed"));
            m_DescriptorManager = std::make_unique<DescriptorManager>(std::move(*Heap));
        }

        // ── Built-in command list ────────────────────────────────────────
        m_GraphicsCommandList = std::make_unique<CommandList>(m_Device, *m_DescriptorManager);
        m_GraphicsCommandList->SetSwapchain(m_Swapchain);
        m_GraphicsCommandList->SetCommittedImageStates(&m_CommittedImageStates);
        LogInfo("Built-in graphics command list initialised");

        return {};
    }

    // ── Frame lifecycle ────────────────────────────────────────────────

    /// Wait until the GPU has caught up to this frame slot, then acquire
    /// the next swapchain image.
    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override {
        // CPU-GPU sync: wait for the timeline semaphore to reach the value
        // from N frames ago (when this slot was last signalled).
        uint64_t WaitValue = m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue;
        if (auto R = m_Timeline.Wait(WaitValue); !R)
            return std::unexpected(R.error().Append("BeginFrame: timeline wait failed"));

        // Free any GPU resources whose transfer operations have completed.
        m_DeletionQueue.Tick();

        auto& PresentCompleteSema = m_FrameContext[m_CurrentFrame].PresentComplete;
        auto  AcquireRes          = m_Swapchain.AcquireNextImage(PresentCompleteSema);
        while (AcquireRes == vk::Result::eErrorOutOfDateKHR || AcquireRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(R.error().Append("Swapchain recreation failed after AcquireNextImage error"));

            // The swapchain now has new VkImage handles — re-register them in
            // the committed-state map so the first-use barrier in the next
            // BeginRendering has a correct srcStageMask (eColorAttachmentOutput)
            // that chains with the AcquireNextImage semaphore wait stage.
            RegisterSwapchainImages();

            // AcquireNextImage signals the semaphore even when returning
            // eSuboptimalKHR.  A signaled semaphore cannot be re-used for another
            // acquire — allocate a fresh unsignaled one for the retry.
            auto NewSema = m_Device.createSemaphore({});
            if (NewSema.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create present-complete semaphore for retry"));
            m_FrameContext[m_CurrentFrame].PresentComplete = std::move(NewSema.value);
            // Re-acquire after recreation
            AcquireRes = m_Swapchain.AcquireNextImage(m_FrameContext[m_CurrentFrame].PresentComplete);
        }
        if (AcquireRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(
                Core::Format("AcquireNextImage failed after swapchain recreation: {}", vk::to_string(AcquireRes))));
        }

        // ── Set the active command buffer for this frame ─────────────────
        m_GraphicsCommandList->SetActiveCommandBuffer(m_FrameContext[m_CurrentFrame].CommandBuffer);

        return {};
    }

    /// Submit collected command buffers for the current frame and present.
    /// Returns false if the swapchain needs recreation.
    [[nodiscard]] auto EndFrame(std::span<RHI::CommandList*> CommandLists)
        -> std::expected<void, ErrorMessage> override {
        // ── Commit per-CommandList local image states to queue-committed state ──
        for (auto* CmdList : CommandLists) {
            auto* VKCtx = static_cast<CommandList*>(CmdList);
            for (const auto& [Key, State] : VKCtx->GetLocalImageStates())
                m_CommittedImageStates[Key] = State;
        }

        // The built-in command list's buffer is set by BeginFrame.
        // Collect it for submission.
        std::vector<vk::CommandBuffer> CommandBuffers;
        CommandBuffers.reserve(CommandLists.size());
        for (auto* Context : CommandLists) {
            auto* CmdList = static_cast<CommandList*>(Context);
            CommandBuffers.push_back(*m_FrameContext[m_CurrentFrame].CommandBuffer);
        }

        std::vector<vk::CommandBufferSubmitInfo> CmdBufferInfos;
        CmdBufferInfos.reserve(CommandBuffers.size());
        for (auto& CmdBuf : CommandBuffers)
            CmdBufferInfos.emplace_back(vk::CommandBufferSubmitInfo{.commandBuffer = CmdBuf});

        // ensure that the swapchain image we want to use
        // is used up for composition
        //
        // NOTE: the meaning of the stage mask here is that
        //       won't check the semaphore unitl the given stage.
        //       So if we choose color attachament output stage,
        //       stages like vertex & fragment can executed before checking.
        //       In other words, we wait on the given stage.
        vk::SemaphoreSubmitInfo PresentCompleteSema{
            .semaphore = *m_FrameContext[m_CurrentFrame].PresentComplete,
            .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        };
        // remember when rendering, we need to assign a swapchain image,
        // so we use the index from swapchain.
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
            .commandBufferInfoCount   = static_cast<uint32_t>(CmdBufferInfos.size()),
            .pCommandBufferInfos      = CmdBufferInfos.data(),
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos    = SignalSemas,
        };

        if (auto R = m_GraphicsQueue.submit2({SubmitInfo2}); R != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format("Queue submit failed: {}", vk::to_string(R))));
        }

        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = TimelineSignalSema.value;

        // ── Present waits on the binary render-complete semaphore ──────────
        auto PresentRes = m_Swapchain.Present(m_GraphicsQueue);
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(R.error().Append("Swapchain recreation failed after Present error"));
            RegisterSwapchainImages();
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
        return {};
    }

    [[nodiscard]] auto CreateVertexBuffer(const void* Data, Uint64 VertexCount, Uint32 Stride)
        -> std::expected<SPtr<RHI::VertexBuffer>, ErrorMessage> override {
        return VertexBuffer::Create(
            m_Allocator, *m_Device, m_ImmediateContext, m_DeletionQueue,
            Data, VertexCount * Stride, Stride, VertexCount);
    }

    [[nodiscard]] auto CreateIndexBuffer(const void* Data, Uint64 IndexCount, bool UseUint16)
        -> std::expected<SPtr<RHI::IndexBuffer>, ErrorMessage> override {
        Uint64 Size = IndexCount * (UseUint16 ? 2ULL : 4ULL);
        return IndexBuffer::Create(
            m_Allocator, *m_Device, m_ImmediateContext, m_DeletionQueue,
            Data, Size, IndexCount, UseUint16);
    }

    [[nodiscard]] auto CreateTexture(const TextureDesc& Desc) -> std::expected<RHI::Texture, ErrorMessage> override {
        (void)Desc;
        return std::unexpected(ErrorMessage("CreateTexture is not implemented yet"));
    }

    [[nodiscard]] auto CreateGraphicsPipeline(const GraphicsPipelineDesc& Desc)
        -> std::expected<SPtr<RHI::GraphicsPipeline>, ErrorMessage> override {
        return GraphicsPipeline::Create(m_Device, Desc, *m_DescriptorManager);
    }

    [[nodiscard]] auto CreateSampler(const SamplerDesc& Desc) -> std::expected<RHI::Sampler, ErrorMessage> override {
        (void)Desc;
        return std::unexpected(ErrorMessage("CreateSampler is not implemented yet"));
    }

    [[nodiscard]] auto DestroyTexture(RHI::Texture TexHdl) -> std::expected<void, ErrorMessage> override {
        // TODO: Implement when Vulkan::Texture wrapper exists
        (void)TexHdl;
        return std::unexpected(ErrorMessage("Vulkan::Texture wrapper not yet implemented"));
    }


    [[nodiscard]] auto DestroySampler(RHI::Sampler SampHdl) -> std::expected<void, ErrorMessage> override {
        (void)SampHdl;
        return std::unexpected(ErrorMessage("DestroySampler is not implemented yet"));
    }

    [[nodiscard]] auto GetCommandList() -> RHI::CommandList& override {
        return *m_GraphicsCommandList;
    }

    auto WaitIdle() -> void override {
        if (*m_Device)
            (void)m_Device.waitIdle();
    }

    auto Shutdown() -> void override {
        WaitIdle();
        auto DrainResult = m_DeletionQueue.Drain();
        if (!DrainResult)
            LogError("{}", DrainResult.error().ToString());
        WaitIdle();
        if (m_Allocator)
            vmaDestroyAllocator(m_Allocator);
    }

  private:
    [[nodiscard]] auto CreateInstance(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
        vk::ApplicationInfo AppInfo{
            .pApplicationName   = "SoulEngine Application",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName        = "SoulEngine",
            .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion         = vk::ApiVersion14,
        };

        std::vector<const char*> Layers;

        auto EnabledInstanceExts = m_Capability.ResolveInstanceExtensions(Context);
        if (!EnabledInstanceExts.has_value())
            return std::unexpected(EnabledInstanceExts.error());

        for (auto* Ext : *EnabledInstanceExts)
            LogDebug("Enabled instance extension: {}", Ext);

        vk::InstanceCreateInfo InstCI{
            .pApplicationInfo        = &AppInfo,
            .enabledLayerCount       = static_cast<uint32_t>(Layers.size()),
            .ppEnabledLayerNames     = Layers.data(),
            .enabledExtensionCount   = static_cast<uint32_t>(EnabledInstanceExts->size()),
            .ppEnabledExtensionNames = EnabledInstanceExts->data(),
        };
        // Traditionally on macOS, vulkan tends to use MoltenVK as an ICD
        // but its implementation of Vulkan Spec is only partial.
        // Vulkan 1.3.216 demands that we declare a Portability flag, along with
        // a KHRPortabilityEnumerationExtension, so that the loader doesn't filter out MoltenVK.
        if (m_Capability.IsInstanceExtensionEnabled(vk::KHRPortabilityEnumerationExtensionName))
            InstCI.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;

        auto InstanceResult = Context.createInstance(InstCI);
        if (InstanceResult.result != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(
                Core::Format("Failed to create Vulkan instance: {}", vk::to_string(InstanceResult.result))));
        }
        m_Instance = std::move(InstanceResult.value);
        return {};
    }

    [[nodiscard]] auto PickPhysicalDevice() -> std::expected<void, ErrorMessage> {
        auto DevicesResult = m_Instance.enumeratePhysicalDevices();
        if (DevicesResult.result != vk::Result::eSuccess || DevicesResult.value.empty())
            return std::unexpected(ErrorMessage("No Vulkan-capable physical devices found"));

        // getProperties2 (Vulkan 1.1+) replaces getProperties.
        // It accepts a pNext chain of extension structs — here
        // PhysicalDeviceDriverProperties — so we can query the ICD
        // driver name/id alongside the base device properties in one call.
        vk::StructureChain<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties> PropsChain;

        for (size_t i = 0; i < DevicesResult.value.size(); ++i) {
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

        // ── Resolve queue families for this physical device ─────────────
        // getQueueFamilyProperties2 (Vulkan 1.1+) wraps the legacy properties
        // in VkQueueFamilyProperties2 and supports a pNext chain for per-family
        // extension structs (global priority, checkpoint properties, etc.).
        auto QueueProps = m_PhysicalDevice.getQueueFamilyProperties2();

        if (auto Res = ResolveQueueFamilies(QueueProps); !Res.has_value())
            return std::unexpected(Res.error());

        return {};
    }

    [[nodiscard]] auto CreateLogicalDevice() -> std::expected<void, ErrorMessage> {
        // ── Gather unique queue families ────────────────────────────────
        std::vector<uint32_t> UniqueFamilies;

        auto AddUnique = [&](uint32_t Family) {
            if (std::ranges::find(UniqueFamilies, Family) == UniqueFamilies.end())
                UniqueFamilies.push_back(Family);
        };

        AddUnique(m_GraphicsFamily);
        AddUnique(m_ComputeFamily);
        AddUnique(m_TransferFamily);

        // ── Build queue create infos ────────────────────────────────────
        float                                  QueuePriority = 1.0f;
        std::vector<vk::DeviceQueueCreateInfo> QueueCIs;
        for (uint32_t Family : UniqueFamilies) {
            QueueCIs.push_back(vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = Family,
                .queueCount       = 1,
                .pQueuePriorities = &QueuePriority,
            });
        }

        auto EnabledDeviceExts = m_Capability.ResolveDeviceExtensionsAndFeatures(m_PhysicalDevice);
        if (!EnabledDeviceExts.has_value())
            return std::unexpected(EnabledDeviceExts.error());

        auto& [DevExts, Feats2] = *EnabledDeviceExts;

        for (auto* Ext : DevExts)
            LogDebug("Enabled device extension: {}", Ext);

        // Previous implementations of Vulkan made a distinction between
        // instance and device-specific validation layers.
        // But now the `enabledLayerCount` and `ppEnabledLayerNames` fields of
        // vk::DeviceCreateInfo are ignored by up-to-date implementations.
        vk::DeviceCreateInfo DevCI{
            .pNext                   = &Feats2,
            .queueCreateInfoCount    = static_cast<uint32_t>(QueueCIs.size()),
            .pQueueCreateInfos       = QueueCIs.data(),
            .enabledExtensionCount   = static_cast<uint32_t>(DevExts.size()),
            .ppEnabledExtensionNames = DevExts.data(),
            // Must be nullptr when pNext includes VkPhysicalDeviceFeatures2.
            .pEnabledFeatures        = nullptr,
        };

        auto DevResult = m_PhysicalDevice.createDevice(DevCI);
        if (DevResult.result != vk::Result::eSuccess) {
            return std::unexpected(
                ErrorMessage(Core::Format("Failed to create logical device: {}", vk::to_string(DevResult.result))));
        }
        m_Device = std::move(DevResult.value);

        // ── Retrieve queue handles ──────────────────────────────────────
        m_GraphicsQueue = m_Device.getQueue(m_GraphicsFamily, 0);
        m_ComputeQueue  = m_Device.getQueue(m_ComputeFamily, 0);
        m_TransferQueue = m_Device.getQueue(m_TransferFamily, 0);

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
        if (m_Capability.IsDeviceExtensionEnabled(vk::EXTMemoryBudgetExtensionName))
            VmaInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        VkResult Result = vmaCreateAllocator(&VmaInfo, &m_Allocator);
        if (Result != VK_SUCCESS)
            return std::unexpected(ErrorMessage(
                Core::Format("Failed to create VMA allocator: {}", vk::to_string(static_cast<vk::Result>(Result)))));
        return {};
    }

    [[nodiscard]] auto CreateSurface(GLFWwindow* Window) -> std::expected<void, ErrorMessage> {
        VkSurfaceKHR RawSurface = VK_NULL_HANDLE;
        VkResult Result = glfwCreateWindowSurface(static_cast<VkInstance>(*m_Instance), Window, nullptr, &RawSurface);
        if (Result != VK_SUCCESS) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(ErrorMessage(Core::Format("glfwCreateWindowSurface failed ({}): {}",
                                                             vk::to_string(static_cast<vk::Result>(Result)),
                                                             Desc ? Desc : "unknown error")));
        }

        m_Surface = vk::raii::SurfaceKHR(m_Instance, RawSurface);
        return {};
    }

    [[nodiscard]] auto ResolveQueueFamilies(std::span<const vk::QueueFamilyProperties2> QueueProps)
        -> std::expected<void, ErrorMessage> {
        // ── Dump available queue families ─────────────────────────────────
        // A queue family maps to a hardware scheduler entry — e.g. the graphics
        // front-end, the DMA engine, or a compute-only pipeline. Different families
        // may share the same flags (e.g. M-series GPUs expose several identical
        // Graphics|Compute|Transfer families), or they may expose dedicated
        // capability (e.g. a discrete GPU may have a dedicated Transfer-only
        // family for async DMA).
        //
        // `queueCount` is the maximum number of queues that can be created from
        // this family. Queues within the same family share the same hardware
        // pipeline but can execute concurrently. For example, queueCount=4 means
        // you can vkGetDeviceQueue(…, index=0..3) and submit separate command
        // streams that the GPU scheduler will overlap.
        //
        // Queue families are declared by the ICD (installable client driver),
        // not directly by the hardware. The same physical GPU can expose different
        // family layouts under different ICDs — e.g. MoltenVK may report 4
        // identical families on Apple silicon while a native Vulkan driver on the
        // same device may report a different arrangement. The ICD decides how to
        // map the underlying hardware scheduling capabilities into Vulkan's queue
        // family abstraction.
        for (size_t i = 0; i < QueueProps.size(); ++i) {
            LogDebug("Queue family [{}]: count={}, flags={}",
                     i,
                     QueueProps[i].queueFamilyProperties.queueCount,
                     vk::to_string(QueueProps[i].queueFamilyProperties.queueFlags));
        }

        // ── Graphics + Present (require a single queue family that supports both) ─
        // Surface was created before PickPhysicalDevice, so it is guaranteed
        // to be valid here. We require one queue family with both graphics and
        // present capability; separate families are not supported.
        {
            for (size_t i = 0; i < QueueProps.size(); ++i) {
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

        // ── Compute (prefer async compute: compute-only or graphics+compute) ─
        // 1st pass: dedicated compute queue (has Compute but NOT Graphics)
        auto ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
        });
        // 2nd pass: any compute-capable queue
        if (ComputeIt == QueueProps.end()) {
            ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{};
            });
        }
        if (ComputeIt == QueueProps.end())
            return std::unexpected(ErrorMessage("No compute-capable queue family found"));
        m_ComputeFamily = static_cast<uint32_t>(std::distance(QueueProps.begin(), ComputeIt));

        // ── Transfer (prefer dedicated transfer queue) ──────────────────
        // 1st pass: dedicated transfer (has Transfer but NOT Graphics and NOT Compute)
        auto TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) == vk::QueueFlags{};
        });
        // 2nd pass: transfer without graphics
        if (TransferIt == QueueProps.end()) {
            TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                       (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
            });
        }
        // 3rd pass: any transfer-capable queue
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

    /// Register all swapchain images in the committed-state map so the
    /// first-use barrier in BeginRendering has a correct srcStageMask that
    /// chains with the AcquireNextImage semaphore wait stage
    /// (eColorAttachmentOutput).
    auto RegisterSwapchainImages() -> void {
        for (uint32_t i = 0; i < m_Swapchain.GetImageCount(); ++i) {
            auto Image                    = m_Swapchain.GetImage(i);
            m_CommittedImageStates[Image] = ImageState{
                .stage  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .access = vk::AccessFlagBits2::eNone,
                .layout = vk::ImageLayout::eUndefined,
            };
        }
    }

    // ── RAII resources ─────────────────────────────────────────────────────

    vk::raii::Instance       m_Instance       = nullptr;
    vk::raii::SurfaceKHR     m_Surface        = nullptr;
    vk::raii::PhysicalDevice m_PhysicalDevice = nullptr;
    vk::raii::Device         m_Device         = nullptr;

    // ── Capability (extensions + feature chain, persists for device lifetime) ─
    Capability m_Capability;

    // ── Cached raw state ───────────────────────────────────────────────────

    uint32_t m_GraphicsFamily = vk::QueueFamilyIgnored;
    uint32_t m_ComputeFamily  = vk::QueueFamilyIgnored;
    uint32_t m_TransferFamily = vk::QueueFamilyIgnored;

    vk::raii::Queue m_GraphicsQueue = nullptr;
    vk::raii::Queue m_ComputeQueue  = nullptr;
    vk::raii::Queue m_TransferQueue = nullptr;

    ImmediateContext         m_ImmediateContext;
    DeferredDeletionQueue    m_DeletionQueue;

    uint32_t m_FramesInFlight = 2;
    uint32_t m_CurrentFrame   = 0;
    bool     m_Validation     = false;

    // ── Swapchain & sync ──────────────────────────────────────────────────

    Swapchain         m_Swapchain;
    VmaAllocator      m_Allocator = nullptr;
    TimelineSemaphore m_Timeline;

    // ── Main command pool (owned here, borrowed by CommandLists) ─────────
    vk::raii::CommandPool m_MainCommandPool = nullptr;

    std::vector<FrameContext> m_FrameContext;

    // ── Global descriptor manager ─────────────────────────────────────────
    Core::UPtr<DescriptorManager> m_DescriptorManager = nullptr;

    // ── Built-in graphics command list ───────────────────────────────────

    Core::UPtr<CommandList> m_GraphicsCommandList = nullptr;

    // ── Resource registries ─────────────────────────────────────────────────


    // ── Barrier state tracking ───────────────────────────────────────────

    /// Queue-committed image state map — reflects GPU state after all
    /// previously submitted work.  Keyed by vk::Image (Vulkan-Hpp provides
    /// std::hash and operator== for handle types).
    /// Snapshot by CommandList::Begin(), written back in EndFrame().
    std::unordered_map<vk::Image, ImageState> m_CommittedImageStates;

    /// TODO: m_CommittedBufferStates for buffer barrier tracking.
};

} // namespace SoulEngine::RHI::Vulkan
