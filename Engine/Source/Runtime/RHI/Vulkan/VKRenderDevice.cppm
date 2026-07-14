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

import RHI; // RHI::CommandList, RHI::Pass, etc.
import vulkan;
import std;

import :Swapchain;
import :Command;
import :Types;
import :Semaphore;
import :Buffer;
import :FrameContext;
import :Capability;
import :ImmediateContext;
import :TransferCompletionQueue;
import :Descriptor;
import :Pipeline;
import :Sampler;
import :Texture;
import :DeletionQueue;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

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

        // ── Transfer Completion Queue ──────────────────────────────────────
        // Must be created before ImmediateContext — ImmediateContext borrows it
        auto CompletionQueue = TransferCompletionQueue::Create(m_Device);
        if (!CompletionQueue)
            return std::unexpected(CompletionQueue.error().Append("TransferCompletionQueue creation failed"));
        m_TransferCompletionQueue = std::move(*CompletionQueue);

        // ── Immediate Context ──────────────────────────────────────────────
        auto ImmCtx = ImmediateContext::Create(m_Device, m_TransferQueue, m_TransferFamily, m_TransferCompletionQueue);
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

        // ── Deletion Queue ───────────────────────────────────────────────
        m_DeletionQueue = DeletionQueue{m_Timeline};

        // ── FrameContext ───────────────────────────────────────────────────
        // Each frame slot gets its own Pool, PrimaryBuffer, and SubPool.
        m_FrameContext.clear();
        m_FrameContext.reserve(m_FramesInFlight);
        for (uint32_t i = 0; i < m_FramesInFlight; ++i) {
            auto FCRes = FrameContext::Create(m_Device, m_GraphicsFamily);
            if (!FCRes)
                return std::unexpected(FCRes.error().Append("FrameContext creation failed"));
            m_FrameContext.push_back(std::move(*FCRes));
        }

        // ── Constant arena ───────────────────────────────────────────────
        const auto ConstantArenaCapacity = Cfg.RhiVulkan.ConstantArenaBufferSize.value_or(4096);
        if (ConstantArenaCapacity > std::numeric_limits<Uint64>::max() / m_FramesInFlight)
            return std::unexpected(ErrorMessage("UniformBufferArena total buffer size overflow"));

        auto ConstantArena =
            UniformBufferArena::Create(static_cast<Uint64>(ConstantArenaCapacity) * m_FramesInFlight, *m_Device, m_Allocator);
        if (!ConstantArena)
            return std::unexpected(ConstantArena.error().Append("UniformBufferArena creation failed"));
        m_ConstantArena = std::move(*ConstantArena);

        // ── Pre-register swapchain images in the committed state map ─────────
        RegisterSwapchainImages();

        m_MaxTextures = ConfigManager::Get().GetConfig().RhiVulkan.MaxTextures.value_or(4096);

        // ── Global descriptor manager ─────────────────────────────────────
        {
            auto Heap = DescriptorManager::Create(m_Device, m_FramesInFlight);
            if (!Heap)
                return std::unexpected(Heap.error().Append("DescriptorManager creation failed"));
            m_DescriptorManager = std::make_unique<DescriptorManager>(std::move(*Heap));
        }

        return {};
    }

    // ── Frame lifecycle — private ─────────────────────────────────────

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
        m_TransferCompletionQueue.Tick();

        // Retire GPU resources whose frame-timeline token has passed.
        m_DeletionQueue.Tick();

        auto& PresentCompleteSema = m_FrameContext[m_CurrentFrame].PresentComplete;
        auto  AcquireRes          = m_Swapchain.AcquireNextImage(PresentCompleteSema);
        while (AcquireRes == vk::Result::eErrorOutOfDateKHR || AcquireRes == vk::Result::eSuboptimalKHR) {
            auto R = m_Swapchain.Recreate();
            if (!R)
                return std::unexpected(R.error().Append("Swapchain recreation failed after AcquireNextImage error"));

            RegisterSwapchainImages();

            auto NewSema = m_Device.createSemaphore({});
            if (NewSema.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create present-complete semaphore for retry"));
            m_FrameContext[m_CurrentFrame].PresentComplete = std::move(NewSema.value);
            AcquireRes = m_Swapchain.AcquireNextImage(m_FrameContext[m_CurrentFrame].PresentComplete);
        }
        if (AcquireRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(
                Core::Format("AcquireNextImage failed after swapchain recreation: {}", vk::to_string(AcquireRes))));
        }

        // ── Begin primary command buffer (skeleton — only ExecuteCommands) ──
        auto& Primary = m_FrameContext[m_CurrentFrame].PrimaryBuffer;
        Primary.reset({});
        vk::CommandBufferBeginInfo PrimaryBegin{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        if (auto R = Primary.begin(PrimaryBegin); R != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Core::Format("BeginFrame: primary CB begin failed: {}", vk::to_string(R))));

        return {};
    }

    [[nodiscard]] auto EndFrame() -> std::expected<void, ErrorMessage> {
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        // ── End primary ─────────────────────────────────────────────────
        if (auto R = Primary.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Core::Format("EndFrame: primary CB end failed: {}", vk::to_string(R))));

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
            return std::unexpected(ErrorMessage(Core::Format("Queue submit failed: {}", vk::to_string(R))));
        }

        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = TimelineSignalSema.value;

        // ── Present ────────────────────────────────────────────────────────
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

    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return m_CurrentFrame;
    }

    [[nodiscard]] auto CreateVertexBuffer(const VertexBufferDesc& Desc)
        -> std::expected<RHI::VertexBufferCreateResult, ErrorMessage> override {
        return VertexBuffer::Create(
            Desc, m_Allocator, *m_Device, m_ImmediateContext, m_TransferCompletionQueue, m_DeletionQueue);
    }

    [[nodiscard]] auto CreateIndexBuffer(const IndexBufferDesc& Desc)
        -> std::expected<RHI::IndexBufferCreateResult, ErrorMessage> override {
        return IndexBuffer::Create(
            Desc, m_Allocator, *m_Device, m_ImmediateContext, m_TransferCompletionQueue, m_DeletionQueue);
    }

    [[nodiscard]] auto CreateConstantBuffer(const ConstantBufferDesc& Desc)
        -> std::expected<UPtr<RHI::ConstantBuffer>, ErrorMessage> override {
        if (Desc.Size == 0)
            return std::unexpected(ErrorMessage("CreateConstantBuffer: size must be greater than zero"));

        std::vector<Uint32> Offsets;
        Offsets.reserve(m_FramesInFlight);
        // One logical ConstantBuffer needs one backing arena slot per
        // frame-in-flight so writes for the current frame never overwrite
        // constant data still referenced by older GPU submissions.
        for (Uint32 FrameIndex = 0; FrameIndex < m_FramesInFlight; ++FrameIndex) {
            auto Offset = m_ConstantArena.Allocate(Desc.Size);
            if (!Offset)
                return std::unexpected(Offset.error().Append("CreateConstantBuffer: arena offset allocation failed"));
            Offsets.push_back(*Offset);
        }

        return std::make_unique<Vulkan::ConstantBuffer>(Desc, std::move(Offsets));
    }

    [[nodiscard]] auto CreateSampler(const SamplerDesc& Desc)
        -> std::expected<UPtr<RHI::Sampler>, ErrorMessage> override {
        return Sampler::Create(Desc, m_Device, m_DeletionQueue);
    }

    [[nodiscard]] auto CreateSampledTexture(const SampledTextureDesc& Desc)
        -> std::expected<RHI::SampledTextureCreateResult, ErrorMessage> override {
        return SampledTexture::Create(Desc,
                                      m_Allocator,
                                      m_Device,
                                      m_ImmediateContext,
                                      m_TransferCompletionQueue,
                                      m_DeletionQueue);
    }

    [[nodiscard]] auto CreateRenderTarget(const RenderTargetDesc& Desc)
        -> std::expected<RHI::RenderTargetCreateResult, ErrorMessage> override {
        return RenderTarget::Create(Desc, m_Allocator, m_Device, m_DeletionQueue);
    }

    [[nodiscard]] auto CreateGraphicsPipeline(const GraphicsPipelineDesc& Desc)
        -> std::expected<UPtr<RHI::GraphicsPipeline>, ErrorMessage> override {
        return GraphicsPipeline::Create(m_Device, Desc, m_MaxTextures, m_DeletionQueue);
    }

    [[nodiscard]] auto IsGpuComplete(GpuCompletionToken Token) -> bool override {
        return m_TransferCompletionQueue.IsComplete(Token);
    }

    auto WaitIdle() -> void override {
        if (*m_Device)
            (void)m_Device.waitIdle();
    }

    auto Shutdown() -> void override {
        WaitIdle();
        auto TransferDrain = m_TransferCompletionQueue.Drain();
        if (!TransferDrain)
            LogError("{}", TransferDrain.error().ToString());
        auto DeletionDrain = m_DeletionQueue.Drain();
        if (!DeletionDrain)
            LogError("{}", DeletionDrain.error().ToString());
        WaitIdle();
        // Destroy VMA-backed buffers before vmaDestroyAllocator.
        m_ConstantArena = {};
        m_FrameContext.clear();
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

        auto EnabledInstanceExts = Capability::Get().ResolveInstanceExtensions(Context);
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
        if (Capability::Get().IsInstanceExtensionEnabled(vk::KHRPortabilityEnumerationExtensionName))
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

        auto EnabledDeviceExts = Capability::Get().ResolveDeviceExtensionsAndFeatures(m_PhysicalDevice);
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
                ErrorMessage(Core::Format("Failed to create logical device: {}", vk::to_string(DevResult.result))));
        }
        m_Device = std::move(DevResult.value);

        // ── Verify required features ────────────────────────────────────
        const auto& V13 = Capability::Get().GetFeatures<vk::PhysicalDeviceVulkan13Features>();
        if (!V13.synchronization2)
            return std::unexpected(ErrorMessage("synchronization2 feature not supported by device"));
        if (!V13.dynamicRendering)
            return std::unexpected(ErrorMessage("dynamicRendering feature not supported by device"));

        m_GraphicsQueue = m_Device.getQueue(m_GraphicsFamily, 0);
        m_ComputeQueue  = m_Device.getQueue(m_ComputeFamily, 0);
        m_TransferQueue = m_Device.getQueue(m_TransferFamily, 0);

        Capability::Get().ResolveDeviceProperties(m_PhysicalDevice);

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
        if (Capability::Get().IsDeviceExtensionEnabled(vk::EXTMemoryBudgetExtensionName))
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
        for (size_t i = 0; i < QueueProps.size(); ++i) {
            LogDebug("Queue family [{}]: count={}, flags={}",
                     i,
                     QueueProps[i].queueFamilyProperties.queueCount,
                     vk::to_string(QueueProps[i].queueFamilyProperties.queueFlags));
        }

        // Graphics + Present
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
            m_CommittedImageStates[Image] = ImageState{
                .stage  = vk::PipelineStageFlagBits2::eNone,
                .access = vk::AccessFlagBits2::eNone,
                .layout = vk::ImageLayout::eUndefined,
            };
        }
    }

    [[nodiscard]] auto ValidateCommandList(const RHI::CommandList& CmdList) const -> std::expected<void, ErrorMessage> {
        if (CmdList.Passes.empty() && !CmdList.PresentSource)
            return {};

        if (!CmdList.PresentSource)
            return std::unexpected(ErrorMessage("Execute: command list with passes must specify PresentSource"));

        for (const auto& Pass : CmdList.Passes) {
            if (!Pass.Desc.ColorAttachment.TexturePtr)
                return std::unexpected(
                    ErrorMessage("Execute: pass color attachment must be an explicit render target"));

            const RHI::GraphicsPipeline* BoundPipeline = nullptr;
            for (const auto& Cmd : Pass.Commands) {
                auto ValidateCommand = [&BoundPipeline](const auto& DrawCmd) -> std::expected<void, ErrorMessage> {
                    using CommandType = std::decay_t<decltype(DrawCmd)>;

                    if constexpr (std::is_same_v<CommandType, RHI::SetGraphicsPipelineCmd>) {
                        if (!DrawCmd.PipelinePtr)
                            return std::unexpected(
                                ErrorMessage("Execute: SetGraphicsPipeline is missing graphics pipeline"));
                        BoundPipeline = DrawCmd.PipelinePtr;
                    } else if constexpr (std::is_same_v<CommandType, RHI::PushConstantsCmd>) {
                        if (!DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage("Execute: PushConstants is missing graphics pipeline"));
                        if (BoundPipeline != DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage(
                                "Execute: PushConstants pipeline does not match the currently bound graphics pipeline"));
                        if (DrawCmd.Data.empty())
                            return std::unexpected(ErrorMessage("Execute: PushConstants data is empty"));
                    } else if constexpr (std::is_same_v<CommandType, RHI::BindShaderParametersCmd>) {
                        if (!DrawCmd.PipelinePtr)
                            return std::unexpected(
                                ErrorMessage("Execute: BindShaderParameters is missing graphics pipeline"));
                        if (BoundPipeline != DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage(
                                "Execute: BindShaderParameters pipeline does not match the currently bound graphics pipeline"));
                        if (DrawCmd.Parameters.GetSets().empty())
                            return std::unexpected(ErrorMessage("Execute: BindShaderParameters has no parameter sets"));
                    } else if constexpr (std::is_same_v<CommandType, RHI::DrawIndexedCmd>) {
                        if (!DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing graphics pipeline"));
                        if (BoundPipeline != DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage(
                                "Execute: indexed draw pipeline does not match the currently bound graphics pipeline"));
                        if (!DrawCmd.VertexBufferPtr)
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing vertex buffer"));
                        if (!DrawCmd.IndexBufferPtr)
                            return std::unexpected(ErrorMessage("Execute: indexed draw is missing index buffer"));
                    } else if constexpr (std::is_same_v<CommandType, RHI::DrawCmd>) {
                        if (!DrawCmd.PipelinePtr)
                            return std::unexpected(ErrorMessage("Execute: draw is missing graphics pipeline"));
                        if (BoundPipeline != DrawCmd.PipelinePtr)
                            return std::unexpected(
                                ErrorMessage("Execute: draw pipeline does not match the currently bound graphics pipeline"));
                        if (!DrawCmd.VertexBufferPtr)
                            return std::unexpected(ErrorMessage("Execute: draw is missing vertex buffer"));
                    }

                    return {};
                };

                if (auto R = std::visit(ValidateCommand, Cmd); !R)
                    return std::unexpected(R.error());
            }
        }

        return {};
    }

    auto RecordPresentBlit(vk::raii::CommandBuffer& Buf, RHI::RenderTarget* Source) -> void {
        auto& SrcRT = static_cast<const Vulkan::RenderTarget&>(*Source);

        const auto SrcImage = SrcRT.GetVkImage();
        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());

        TransitionImage(Buf,
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
        auto& DstState = m_CommittedImageStates[DstImage];
        DstState.stage = vk::PipelineStageFlagBits2::eTransfer;
        DstState.access = vk::AccessFlagBits2::eNone;

        TransitionImage(Buf,
                        m_CommittedImageStates,
                        DstImage,
                        vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferWrite,
                        vk::ImageLayout::eTransferDstOptimal,
                        true);

        const auto DstExtent = m_Swapchain.GetExtent();
        vk::ImageBlit BlitRegion{
            .srcSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                               .mipLevel       = 0,
                               .baseArrayLayer = 0,
                               .layerCount     = 1},
            .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                       vk::Offset3D{static_cast<Int32>(SrcRT.GetWidth()),
                                                                    static_cast<Int32>(SrcRT.GetHeight()),
                                                                    1}},
            .dstSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                               .mipLevel       = 0,
                               .baseArrayLayer = 0,
                               .layerCount     = 1},
            .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                       vk::Offset3D{static_cast<Int32>(DstExtent.width),
                                                                    static_cast<Int32>(DstExtent.height),
                                                                    1}},
        };

        Buf.blitImage(SrcImage,
                      vk::ImageLayout::eTransferSrcOptimal,
                      DstImage,
                      vk::ImageLayout::eTransferDstOptimal,
                      {BlitRegion},
                      vk::Filter::eNearest);

        TransitionImage(Buf,
                        m_CommittedImageStates,
                        DstImage,
                        vk::PipelineStageFlagBits2::eBottomOfPipe,
                        vk::AccessFlagBits2::eNone,
                        vk::ImageLayout::ePresentSrcKHR,
                        false);
    }

    // CommandVisitor lives in VKCommand.cppm — imported via :Command partition.

    // ═════════════════════════════════════════════════════════════════════════════
    // Execute — consume CommandList, record and submit
    // ═════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] auto Execute(const RHI::CommandList& CmdList) -> std::expected<void, ErrorMessage> override {
        if (auto R = ValidateCommandList(CmdList); !R)
            return std::unexpected(R.error());
        if (CmdList.Passes.empty() && !CmdList.PresentSource)
            return {};

        if (auto R = BeginFrame(); !R)
            return R;
        // Frame token for usage tracking — this frame's signal value on the timeline
        const Uint64                  FrameTokenValue = m_Timeline.NextValue();
        const RHI::GpuCompletionToken FrameToken{.Id = FrameTokenValue};
        RHI::UsageVisitor             UsageTracker{.CurrentToken = FrameToken};
        UsageTracker.StampPresentSource(CmdList.PresentSource);

        std::vector<vk::CommandBuffer> Secondaries;
        Secondaries.reserve(CmdList.Passes.size());

        for (const auto& Pass : CmdList.Passes) {
            // ── Update usage tokens on all resources referenced by this pass ──
            UsageTracker.StampPassAttachments(Pass.Desc);
            for (const auto& Cmd : Pass.Commands)
                std::visit(UsageTracker, Cmd);

            // Allocate per-pass secondary from the frame's sub-pool
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
                    ErrorMessage(Core::Format("Execute: secondary CB begin failed: {}", vk::to_string(R))));

            // Begin rendering scope from Pass desc
            {
                auto           ImageStateCopy = m_CommittedImageStates;
                CommandVisitor Visitor{
                    .Buf           = SecBuf,
                    .LocalStates   = ImageStateCopy,
                    .Descriptors   = m_DescriptorManager.get(),
                    .ConstantArena = &m_ConstantArena,
                    .FrameIndex    = m_CurrentFrame,
                };
                Visitor.BeginPass(Pass.Desc);
                for (const auto& Cmd : Pass.Commands) {
                    std::visit(Visitor, Cmd);
                    if (Visitor.Error)
                        return std::unexpected(*Visitor.Error);
                }
                Visitor.EndPass();
                m_CommittedImageStates = std::move(ImageStateCopy);
            }
            if (auto R = SecBuf.end(); R != vk::Result::eSuccess)
                return std::unexpected(
                    ErrorMessage(Core::Format("Execute: secondary CB end failed: {}", vk::to_string(R))));
            Secondaries.push_back(static_cast<vk::CommandBuffer>(*SecBuf));
        }

        // Primary CB was already prepared in BeginFrame — just execute secondaries
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        if (!Secondaries.empty())
            Primary.executeCommands(Secondaries);
        RecordPresentBlit(Primary, CmdList.PresentSource);
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
            return std::unexpected(ErrorMessage(Core::Format("Queue submit failed: {}", vk::to_string(R))));

        m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue = FrameTokenValue;

        auto PresentRes = m_Swapchain.Present(m_GraphicsQueue);
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            if (auto R = m_Swapchain.Recreate(); !R)
                return std::unexpected(R.error().Append("Swapchain recreation failed after Present error"));
            RegisterSwapchainImages();
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
        return {};
    }

    // ── RAII resources ─────────────────────────────────────────────────────

    vk::raii::Instance       m_Instance       = nullptr;
    vk::raii::SurfaceKHR     m_Surface        = nullptr;
    vk::raii::PhysicalDevice m_PhysicalDevice = nullptr;
    vk::raii::Device         m_Device         = nullptr;

    uint32_t m_GraphicsFamily = vk::QueueFamilyIgnored;
    uint32_t m_ComputeFamily  = vk::QueueFamilyIgnored;
    uint32_t m_TransferFamily = vk::QueueFamilyIgnored;

    vk::raii::Queue m_GraphicsQueue = nullptr;
    vk::raii::Queue m_ComputeQueue  = nullptr;
    vk::raii::Queue m_TransferQueue = nullptr;

    ImmediateContext        m_ImmediateContext;
    TransferCompletionQueue m_TransferCompletionQueue;

    uint32_t m_FramesInFlight = 2;
    uint32_t m_CurrentFrame   = 0;
    bool     m_Validation     = false;

    // ── Swapchain & sync ──────────────────────────────────────────────────

    Swapchain         m_Swapchain;
    VmaAllocator      m_Allocator = nullptr;
    TimelineSemaphore m_Timeline;

    DeletionQueue m_DeletionQueue; // re-initialized after m_Timeline created

    std::vector<FrameContext> m_FrameContext;
    UniformBufferArena        m_ConstantArena = {};

    // ── Global descriptor manager ─────────────────────────────────────────
    Core::UPtr<DescriptorManager> m_DescriptorManager = nullptr;
    Uint32                        m_MaxTextures        = 4096;

    // ── Barrier state tracking ───────────────────────────────────────────

    std::unordered_map<vk::Image, ImageState> m_CommittedImageStates;
};

} // namespace SoulEngine::RHI::Vulkan
