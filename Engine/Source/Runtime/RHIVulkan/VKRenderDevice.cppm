module;

// For VMA loading procedure, see:
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html Because we use
// `DynamicLoader` from vulkan headers, we define: dynamically fetching pointers using `vkGetInstanceProcAddr`
// and `vkGetDeviceProcAddr`
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>
#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RHIVulkan:RenderDevice;

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
import :Readback;
import :Staging;
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
import :Profiling;
import Core;
import magic_enum;
import WindowSystem;

namespace SoulEngine {

CVar<Int32> CVarRenderFramesInFlight{"Render.FramesInFlight", 2, "Number of frames in flight"};
CVar<Int32> CVarRhiVulkanUniformBeltSize{"RHI.Vulkan.UniformBeltSize",
                                         64 * 1024,
                                         "Total transient uniform belt capacity (bytes); grows on demand, "
                                         "never shrinks. Read once at device init"};
CVar<Int32> CVarRhiVulkanShaderStorageBeltSize{"RHI.Vulkan.ShaderStorageBeltSize",
                                               64 * 1024 * 1024,
                                               "Total transient shader-storage belt capacity (bytes); grows on "
                                               "demand, never shrinks. Read once at device init"};
CVar<Int32> CVarRhiVulkanStagingRingBufferSize{"RHI.Vulkan.StagingRingBufferSize",
                                               16 * 1024 * 1024,
                                               "Initial staging ring capacity for GpuOnly buffer uploads (bytes); "
                                               "grows on demand, never shrinks. Read once at device init"};
CVar<Int32> CVarRhiVulkanASScratchRingSize{"RHI.Vulkan.ASScratchRingSize",
                                           4 * 1024 * 1024,
                                           "Initial acceleration-structure build scratch ring capacity (bytes); "
                                           "grows on demand, never shrinks. Read once at device init"};
CVar<Int32> CVarRhiVulkanASInstanceRingSize{"RHI.Vulkan.ASInstanceRingSize",
                                            64 * 1024,
                                            "Initial TLAS instance ring capacity (bytes); grows on demand, "
                                            "never shrinks. Read once at device init"};

class VulkanRenderDevice final : public RHIRenderDevice {
  public:
    VulkanRenderDevice() {}
    ~VulkanRenderDevice() {
        (void)Shutdown();
    }

    [[nodiscard]] auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> override {
        // Read configuration from the CVars (TOML values were applied at launch)
        m_FramesInFlight = static_cast<uint32_t>(CVarRenderFramesInFlight.Get());

        auto Resources = VulkanResourceContext::Create(WindowSys, m_FramesInFlight);
        if (!Resources)
            return std::unexpected(Resources.error().Append("VulkanResourceContext creation failed"));
        // VulkanResourceContext owns shared Vulkan services and is borrowed by resource factories through
        // this RenderDevice-held pointer.
        m_ResourceContext = std::move(*Resources);
        // Tracy GPU profiling

        // Global descriptor manager
        auto Descriptors =
            VulkanDescriptorManager::Create(m_ResourceContext->GetDevice(), m_ResourceContext->GetDebugUtils());
        if (!Descriptors)
            return std::unexpected(Descriptors.error().Append("VulkanDescriptorManager creation failed"));
        m_DescriptorManager = std::move(*Descriptors);

        auto Swapchain = VulkanSwapchain::Create(*m_ResourceContext);
        if (!Swapchain)
            return std::unexpected(Swapchain.error());
        m_Swapchain = std::move(*Swapchain);
        // VulkanFrameContext

        // Each frame slot gets its own Pool, PrimaryBuffer, and SubPool.
        m_FrameContext.clear();
        m_FrameContext.reserve(m_FramesInFlight);
        for (uint32_t i = 0; i < m_FramesInFlight; ++i) {
            auto FCRes = VulkanFrameContext::Create(*m_ResourceContext, i);
            if (!FCRes)
                return std::unexpected(FCRes.error().Append("VulkanFrameContext creation failed"));
            m_FrameContext.push_back(std::move(*FCRes));
        }

        // Staging belt: the shared frame-start upload staging, borrowed by both managers
        auto Staging = VulkanStagingBelt::Create(*m_ResourceContext,
                                                 static_cast<Uint64>(CVarRhiVulkanStagingRingBufferSize.Get()));
        if (!Staging)
            return std::unexpected(Staging.error().Append("VulkanStagingBelt creation failed"));
        m_StagingBelt = std::move(*Staging);

        // Buffer manager: transient belts, upload front-end, creation dispatch
        auto Buffers = VulkanBufferManager::Create(*m_ResourceContext,
                                                   static_cast<Uint64>(CVarRhiVulkanUniformBeltSize.Get()),
                                                   static_cast<Uint64>(CVarRhiVulkanShaderStorageBeltSize.Get()),
                                                   m_StagingBelt);
        if (!Buffers)
            return std::unexpected(Buffers.error().Append("VulkanBufferManager creation failed"));
        m_BufferManager = std::move(*Buffers);

        // Texture manager: creation dispatch and the upload front-end
        auto Textures = VulkanTextureManager::Create(*m_ResourceContext, m_StagingBelt);
        if (!Textures)
            return std::unexpected(Textures.error().Append("VulkanTextureManager creation failed"));
        m_TextureManager = std::move(*Textures);

        // AS manager: creation, the frame-start build batch, the AS rings
        auto AccelerationStructures =
            VulkanAccelerationStructureManager::Create(*m_ResourceContext,
                                                       static_cast<Uint64>(CVarRhiVulkanASScratchRingSize.Get()),
                                                       static_cast<Uint64>(CVarRhiVulkanASInstanceRingSize.Get()));
        if (!AccelerationStructures)
            return std::unexpected(
                AccelerationStructures.error().Append("VulkanAccelerationStructureManager creation failed"));
        m_AccelerationStructureManager = std::move(*AccelerationStructures);

        if (auto Res = InitializeImGui(WindowSys); !Res)
            return std::unexpected(Res.error().Append("Vulkan Dear ImGui renderer initialization failed"));

        m_NeedsShutdown = true;
        return {};
    }

    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Vulkan;
    }
    // Frame lifecycle — private

    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override {
        // Publish the executing frame slot so frame-slotted payloads (TLAS) resolve their internal slot
        // during this frame's command recording.
        m_ResourceContext->SetCurrentFrameIndex(m_CurrentFrame);
        // CPU-GPU sync: wait for the timeline semaphore to reach the value from N frames ago (when this slot
        // was last signalled).
        uint64_t WaitValue = m_FrameContext[m_CurrentFrame].SubmissionCompleteTimelineValue;
        m_ResourceContext->GetTimeline().Wait(WaitValue);

        // GPU done with this frame slot — safe to free scratch secondaries.
        m_FrameContext[m_CurrentFrame].ScratchSecondaries.clear();

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

        // Begin primary command buffer (AS phase, the frame's pass secondary, present)
        auto& Primary = m_FrameContext[m_CurrentFrame].PrimaryBuffer;
        Primary.reset({});
        vk::CommandBufferBeginInfo PrimaryBegin{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        if (auto R = Primary.begin(PrimaryBegin); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("BeginFrame: primary CB begin failed: {}", vk::to_string(R))));
        CollectGpu(*m_ResourceContext, Primary);

        // Transient slices recycle per frame; drop last frame's buffer access states so every range starts
        // untracked (the frame-slot fence already guarantees the previous occupant's GPU work has completed).
        //
        // TODO: might be a issue when we need barrier for a non-transient buffer.
        m_ResourceContext->GetBufferTracker().Reset();

        return {};
    }

    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return m_CurrentFrame;
    }

    /// @brief Create a buffer; forwards to the buffer manager.
    ///
    /// Thin forward: all buffer concerns — Desc validation, creation dispatch, uploads, transient belts —
    /// live in VulkanBufferManager (:Buffer).
    [[nodiscard]] auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> override {
        return m_BufferManager.CreateBuffer(Name, Desc);
    }

    /// @brief Create a readback object; ring depth = frames in flight + 1 (see VulkanReadback).
    ///
    /// Synchronous (VMA/vkCreateBuffer are thread-safe), published Ready at return. Creation lives on the
    /// device: readbacks are consumer objects, outside the buffer manager's remit.
    [[nodiscard]] auto CreateReadback(StringView Name, Uint64 SizeBytes)
        -> std::expected<RHIRef<RHIReadback>, ErrorMessage> override {
        auto Result = VulkanReadback::Create(*m_ResourceContext, Name, SizeBytes, m_FramesInFlight + 1);
        if (!Result)
            return std::unexpected(Result.error());
        auto Resource = RHIRef<RHIReadback>::Create();
        if (auto Publish = Resource.Publish(std::move(*Result), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Upload initial data into a buffer; forwards to the buffer manager.
    ///
    /// Thin forward: upload validation, snapshotting, and the frame-affined task live in VulkanBufferManager
    /// (:Buffer).
    [[nodiscard]] auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        return m_BufferManager.Upload(std::move(Buffer), Data);
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

    /// @brief Create a texture; forwards to the texture manager.
    ///
    /// Thin forward: all texture concerns — creation dispatch and the upload front-end — live in
    /// VulkanTextureManager (:Texture).
    [[nodiscard]] auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> override {
        return m_TextureManager.CreateTexture(Name, Desc);
    }

    [[nodiscard]] auto RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
        -> std::expected<void, ErrorMessage> override {
        return m_DescriptorManager.RegisterBindlessTextures(Textures);
    }

    [[nodiscard]] auto FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const
        -> std::optional<Uint32> override {
        return m_DescriptorManager.FindBindlessTextureIndex(Texture);
    }

    /// @brief Upload initial data into a texture; forwards to the texture manager.
    ///
    /// Thin forward: upload validation, snapshotting, and the frame-affined task live in VulkanTextureManager
    /// (:Texture).
    [[nodiscard]] auto UploadTexture(RHIRef<RHITexture> Texture, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        return m_TextureManager.Upload(std::move(Texture), Data);
    }

    [[nodiscard]] auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override {
        if (!m_ResourceContext)
            return std::unexpected(ErrorMessage("Vulkan shader binding set requires resource context"));

        auto Created = VulkanShaderBindingSet::Create(*m_ResourceContext, m_DescriptorManager, Name, Desc);
        if (!Created)
            return std::unexpected(Created.error());

        auto                      Resource = RHIRef<RHIShaderBindingSet>::Create();
        UPtr<RHIShaderBindingSet> Payload  = std::move(*Created);
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

    [[nodiscard]] auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> override {
        auto Result = VulkanComputePipeline::Create(*m_ResourceContext, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto                     Resource = RHIRef<RHIComputePipeline>::Create();
        UPtr<RHIComputePipeline> Payload  = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override {
        auto Result = VulkanRayTracingPipeline::Create(*m_ResourceContext, m_StagingBelt, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());

        auto                        Resource = RHIRef<RHIRayTracingPipeline>::Create();
        UPtr<RHIRayTracingPipeline> Payload  = std::move(*Result);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Create a BLAS; forwards to the acceleration-structure manager.
    ///
    /// Thin forward: validation and materialization live in VulkanAccelerationStructureManager
    /// (:AccelerationStructure).
    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(StringView                                     Name,
                                                              const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return m_AccelerationStructureManager.CreateBlas(Name, Desc);
    }

    /// @brief Create a TLAS; forwards to the acceleration-structure manager.
    ///
    /// Thin forward: validation and materialization live in VulkanAccelerationStructureManager
    /// (:AccelerationStructure).
    [[nodiscard]] auto CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                           const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return m_AccelerationStructureManager.CreateTlas(Name, Desc);
    }

    /// @brief Enqueue a BLAS build for the frame-start batch; forwards to the acceleration-structure manager.
    ///
    /// Thin forward: validation and the frame-affined task live in VulkanAccelerationStructureManager
    /// (:AccelerationStructure).
    [[nodiscard]] auto BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas)
        -> std::expected<void, ErrorMessage> override {
        return m_AccelerationStructureManager.BuildBlas(std::move(Blas));
    }

    /// @brief Enqueue a TLAS build for the frame-start batch; forwards to the acceleration-structure manager.
    ///
    /// Thin forward: validation and the frame-affined task live in VulkanAccelerationStructureManager
    /// (:AccelerationStructure).
    [[nodiscard]] auto BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas)
        -> std::expected<void, ErrorMessage> override {
        return m_AccelerationStructureManager.BuildTlas(std::move(Tlas));
    }

    [[nodiscard]] auto Tick() -> std::expected<void, ErrorMessage> override {
        m_DescriptorManager.Tick();
        // Frame begin: reserve the frame-begin timeline value ahead of this frame's submissions; the buffer
        // chain retires what the device timeline has passed, then seals the previous frame's pending uploads
        // against it (the next real submission covers the reserved value).
        const Uint64 FrameBeginTimelineValue = m_ResourceContext->GetTimeline().IncreaseHostValue();
        m_BufferManager.Tick(FrameBeginTimelineValue);
        m_StagingBelt.Tick(FrameBeginTimelineValue);
        m_AccelerationStructureManager.Tick(FrameBeginTimelineValue);
        m_ResourceContext->GetDeferredDeletionQueue().Tick();
        return {};
    }

    [[nodiscard]] auto WaitIdle() -> std::expected<void, ErrorMessage> override {
        if (!m_ResourceContext || !*m_ResourceContext->GetDevice())
            return {};
        if (auto R = m_ResourceContext->GetDevice().waitIdle(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("Vulkan device wait idle failed: {}", vk::to_string(R))));
        return {};
    }

    [[nodiscard]] auto Shutdown() -> std::expected<void, ErrorMessage> override {
        std::optional<ErrorMessage> ShutdownError       = std::nullopt;
        const auto                  RecordShutdownError = [&ShutdownError](const ErrorMessage& Error) {
            if (!ShutdownError)
                ShutdownError = Error;
        };

        if (std::exchange(m_NeedsShutdown, false)) {
            if (auto R = WaitIdle(); !R)
                RecordShutdownError(R.error());
            ImGui_ImplVulkan_Shutdown();
            if (auto R = WaitIdle(); !R)
                RecordShutdownError(R.error());
            DrainRHIDeferredDeletions();
        }

        // The tail below requires a live resource context. It runs at most once: the destructor re-enters
        // Shutdown() after Destroy() already released m_ResourceContext, and the failed-Initialize() path can
        // reach the destructor without a context at all. m_NeedsShutdown is only set after m_ResourceContext,
        // so no recorded error is dropped by this early-out.
        if (!m_ResourceContext)
            return {};

        // Destroy VMA-backed buffers before vmaDestroyAllocator.
        m_Swapchain.Cleanup();
        m_BufferManager.Shutdown();
        m_TextureManager.Shutdown();
        m_AccelerationStructureManager.Shutdown();
        m_StagingBelt = {};
        m_FrameContext.clear();

        // Descriptor sets and their pool must be released while VkDevice is alive.
        m_DescriptorManager.Shutdown();
        // Managers released RHIRef owners above; their Vulkan resource payloads are now pending in the
        // process-wide RHI deferred queue. Drain after releasing those owners so their VkImage/VkBuffer
        // allocations enter the backend queue before the final unconditional flush.
        DrainRHIDeferredDeletions();
        m_ResourceContext->GetDeferredDeletionQueue().Flush();
        // Tracy context must die while VkDevice is still alive.
        m_ResourceContext->DestroyTracyCtx();
        m_ResourceContext.reset();

        if (ShutdownError)
            return std::unexpected(std::move(*ShutdownError));
        return {};
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

        // Dear ImGui exposes a C Vulkan API. These raw values are confined to this third-party adapter
        // boundary.
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
        InitInfo.DescriptorPool      = static_cast<VkDescriptorPool>(m_DescriptorManager.GetDescriptorPool());
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
    [[nodiscard]] auto RecordImGuiPresentationOverlay(vk::raii::CommandBuffer&              Buf,
                                                      const RHIImGuiPresentationOverlayCmd& Overlay)
        -> std::expected<void, ErrorMessage> {
        const LabelScope OverlayLabel{*m_ResourceContext, Buf, "ImGuiOverlay"};
        if (!Overlay.Snapshot || !Overlay.TextureQueue || !Overlay.TextureMutex)
            return std::unexpected(ErrorMessage("ImGui presentation overlay has incomplete state"));
        {
            std::lock_guard Lock(*Overlay.TextureMutex);
            Overlay.TextureQueue->ProcessRequests(&Overlay.Snapshot->DrawData);
        }

        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());
        // The overlay follows the blit and alpha-blends over the scene, so it must load and preserve the
        // existing swapchain color.
        m_ResourceContext->GetImageTracker().Transition(
            Buf,
            DstImage,
            vk::ImageSubresourceRange{.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                      .baseMipLevel   = 0,
                                      .levelCount     = 1,
                                      .baseArrayLayer = 0,
                                      .layerCount     = 1},
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .access = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
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

    /// @brief Materialize one pass's prologue barriers into the primary command buffer.
    ///
    /// Every dst-only state requirement is diffed against the emission-side trackers, and only state changes
    /// emit a native barrier. Recorded into the primary so each barrier sits between the producer's and this
    /// pass's commands in submission order.
    [[nodiscard]] auto EmitPassPrologue(vk::raii::CommandBuffer& Primary, const RHIBarrierList& Prologue)
        -> std::expected<void, ErrorMessage> {
        for (const auto& Transition : Prologue.Buffers) {
            const auto* Buffer = Transition.View.Buffer.TryGet();
            if (!Buffer)
                return std::unexpected(ErrorMessage("Execute: prologue references an invalid buffer"));
            const auto&  VulkanBuf = static_cast<const VulkanBuffer&>(*Buffer);
            // Belt slices track at their allocation offset; persistent (imported) buffers track at offset 0.
            const auto*  Prop      = VulkanBuf.GetProp<TransientBufferProp>();
            const Uint64 Offset    = Prop ? Prop->Allocation.Offset : 0;
            m_ResourceContext->GetBufferTracker().Transition(
                Primary,
                VulkanBuf.GetVkBuffer(),
                Offset,
                VulkanBuf.GetSize(),
                VulkanBufferState{
                    .stage   = ToVkPipelineStages(Transition.Stage),
                    .access  = ToVkBufferAccess(Transition.View.Usage, Transition.View.Access),
                    .isWrite = magic_enum::enum_flags_test(Transition.View.Access, RHIAccess::Write),
                });
        }
        for (const auto& Transition : Prologue.Textures) {
            const auto* Target = Transition.View.Texture.TryGet();
            if (!Target)
                return std::unexpected(ErrorMessage("Execute: prologue references an invalid texture"));
            const auto& VulkanTarget = static_cast<const VulkanTexture&>(*Target);
            const auto  Aspect       = ToVkImageAspect(VulkanTarget.GetFormat());
            // Ranges are always concrete for the tracker: a whole-chain view resolves against the payload's
            // own mip count here.
            const auto  Range        = vk::ImageSubresourceRange{
                .aspectMask     = Aspect,
                .baseMipLevel   = Transition.View.Range.MipLevelCount == kRHIRemainingSubresources
                                      ? 0
                                      : Transition.View.Range.BaseMipLevel,
                .levelCount     = Transition.View.Range.MipLevelCount == kRHIRemainingSubresources
                                      ? VulkanTarget.GetMipLevelCount()
                                      : Transition.View.Range.MipLevelCount,
                .baseArrayLayer = 0,
                .layerCount     = 1};
            m_ResourceContext->GetImageTracker().Transition(
                Primary,
                VulkanTarget.GetVkImage(),
                Range,
                VulkanImageState{
                    .stage   = ToVkPipelineStages(Transition.Stage),
                    .access  = ToVkTextureAccess(Transition.View.Usage, Transition.View.Access),
                    .layout  = ToVkImageLayout(Transition.View.Usage, Transition.View.Access),
                    .isWrite = magic_enum::enum_flags_test(Transition.View.Access, RHIAccess::Write),
                });
        }
        return {};
    }

    /// @brief Record the frame-start upload batch into a dedicated secondary appended ahead of every pass.
    ///
    /// Entries were produced by the frame-affined UploadBuffer/UploadTexture tasks drained earlier this frame
    /// (staging allocation and host copy already done there); the belt records one copy per entry, the buffer
    /// lane sealed by a single aggregated transfer-write → consumer-read barrier. Runs first in the
    /// frame-start phase because the BLAS batch reads geometry buffers. Recorded unconditionally — an empty
    /// batch simply yields an empty secondary.
    [[nodiscard]] auto RecordUploadWork(vk::raii::CommandBuffer& Primary) -> std::expected<void, ErrorMessage> {
        auto&                         FC = m_FrameContext[m_CurrentFrame];
        vk::CommandBufferAllocateInfo Alloc{
            .commandPool        = *FC.SubPool,
            .level              = vk::CommandBufferLevel::eSecondary,
            .commandBufferCount = 1,
        };
        auto AllocResult = m_ResourceContext->GetDevice().allocateCommandBuffers(Alloc);
        if (AllocResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Execute: failed to allocate upload secondary CB"));
        m_ResourceContext->GetDebugUtils().SetObjectName(
            *AllocResult.value[0], Format("Internal/CommandBuffer/Secondary/Frame{}/UploadBatch", m_CurrentFrame));
        auto& SecBuf = FC.ScratchSecondaries.emplace_back(std::move(AllocResult.value[0]));

        vk::CommandBufferInheritanceInfo Inheritance{};
        vk::CommandBufferBeginInfo       BeginCI{
            .flags            = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
            .pInheritanceInfo = &Inheritance,
        };
        if (auto R = SecBuf.begin(BeginCI); R != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("Execute: upload secondary CB begin failed: {}", vk::to_string(R))));

        m_StagingBelt.RecordUploadBatch(SecBuf);

        if (auto R = SecBuf.end(); R != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("Execute: upload secondary CB end failed: {}", vk::to_string(R))));
        Primary.executeCommands({static_cast<vk::CommandBuffer>(*SecBuf)});
        return {};
    }

    [[nodiscard]] auto Execute(RenderResult& Result) -> std::expected<void, ErrorMessage> override {
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;

        // Frame-start phase: the buffer upload batch first, then the BLAS batch, then each TLAS build, so the
        // pass list consumes uploads and builds in submission order. Each phase records into its own
        // secondary — the primary stays a pure skeleton. The upload batch records unconditionally; an empty
        // batch costs one empty secondary. The AS phase drains the manager's pending lists (filled by the
        // frame-affined BuildBlas/BuildTlas tasks) and early-outs when empty.
        if (auto R = RecordUploadWork(Primary); !R)
            return std::unexpected(R.error().Append("Execute: upload batch failed"));
        if (auto R = m_AccelerationStructureManager.RecordBuildBatch(FC, m_CurrentFrame, Primary); !R)
            return std::unexpected(R.error().Append("Execute: AS build work failed"));

        // All passes record into ONE frame secondary: the pass producer's writes, each pass's prologue
        // barrier, and the pass's reads stay in a single command-buffer context so synchronization validation
        // resolves hazards at record time (syncval does not resolve cross-secondary boundary hazards against
        // barriers).
        vk::raii::CommandBuffer* SecBuf = nullptr;
        if (!Result.CmdList.Entries.empty()) {
            vk::CommandBufferAllocateInfo Alloc{
                .commandPool        = *m_FrameContext[m_CurrentFrame].SubPool,
                .level              = vk::CommandBufferLevel::eSecondary,
                .commandBufferCount = 1,
            };
            auto AllocResult = m_ResourceContext->GetDevice().allocateCommandBuffers(Alloc);
            if (AllocResult.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Execute: failed to allocate secondary CB"));
            m_ResourceContext->GetDebugUtils().SetObjectName(
                *AllocResult.value[0], Format("Internal/CommandBuffer/Secondary/Frame{}/Passes", m_CurrentFrame));
            SecBuf = &m_FrameContext[m_CurrentFrame].ScratchSecondaries.emplace_back(std::move(AllocResult.value[0]));

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
            if (auto R = SecBuf->begin(BeginCI); R != vk::Result::eSuccess)
                return std::unexpected(
                    ErrorMessage(Format("Execute: secondary CB begin failed: {}", vk::to_string(R))));
        }

        for (std::size_t PassIndex = 0; PassIndex < Result.CmdList.Entries.size(); ++PassIndex) {
            const auto& Entry = Result.CmdList.Entries[PassIndex];
            const auto& Pass  = Entry.Pass;
            if (!Pass)
                return std::unexpected(ErrorMessage(Format("Execute: pass {} is null", PassIndex)));
            auto* Pipeline = Pass->GetPipeline();
            // Transfer passes have no pipeline; all other passes must have one.
            if (Pass->GetType() != RHIPassType::Transfer && !Pipeline)
                return std::unexpected(ErrorMessage(Format("Execute: pass {} has no pipeline", Pass->GetName())));
            if (auto R = Pass->Record(); !R)
                return std::unexpected(R.error().Append(Format("Execute: pass {} recording failed", Pass->GetName())));

            // The graph-declared prologue is materialized ahead of the pass's commands inside the shared
            // frame secondary.
            if (auto R = EmitPassPrologue(*SecBuf, Entry.Prologue); !R)
                return std::unexpected(R.error().Append(Format("Execute: pass {} prologue failed", Pass->GetName())));

            {
                const LabelScope PassLabel{*m_ResourceContext, *SecBuf, Pass->GetName()};
                const auto       RecordCommands = [&](auto& Visitor) -> std::expected<void, ErrorMessage> {
                    if (Visitor.GetError())
                        return std::unexpected(*Visitor.GetError());
                    for (const auto& Cmd : Pass->GetCommands()) {
                        std::visit(Visitor, Cmd);
                        if (Visitor.GetError())
                            return std::unexpected(*Visitor.GetError());
                    }
                    return {};
                };

                std::expected<void, ErrorMessage> RecordResult;
                switch (Pass->GetType()) {
                case RHIPassType::Graphics: {
                    const auto& GraphicsPass = static_cast<const IRHIGraphicsPass&>(*Pass);
                    auto*       GfxPipeline  = static_cast<VulkanGraphicsPipeline*>(Pipeline);
                    if (!GfxPipeline)
                        return std::unexpected(ErrorMessage("Execute: graphics pass has no pipeline"));
                    VulkanGraphicsCmdVisitor Visitor{
                        *GfxPipeline,
                        GraphicsPass.GetAttachments(),
                        *SecBuf,
                        *m_ResourceContext,
                    };
                    RecordResult = RecordCommands(Visitor);
                    break;
                }
                case RHIPassType::Compute: {
                    auto* CompPipeline = static_cast<VulkanComputePipeline*>(Pipeline);
                    if (!CompPipeline)
                        return std::unexpected(ErrorMessage("Execute: compute pass has no pipeline"));
                    VulkanComputeCmdVisitor Visitor{
                        *CompPipeline,
                        *SecBuf,
                        *m_ResourceContext,
                    };
                    RecordResult = RecordCommands(Visitor);
                    break;
                }
                case RHIPassType::RayTracing: {
                    auto* RtPipeline = static_cast<VulkanRayTracingPipeline*>(Pipeline);
                    if (!RtPipeline)
                        return std::unexpected(ErrorMessage("Execute: ray-tracing pass has no pipeline"));
                    VulkanRayTracingCmdVisitor Visitor{
                        *RtPipeline,
                        *SecBuf,
                        *m_ResourceContext,
                    };
                    RecordResult = RecordCommands(Visitor);
                    break;
                }
                case RHIPassType::Transfer: {
                    VulkanTransferCmdVisitor Visitor{*SecBuf, *m_ResourceContext, m_Swapchain};
                    RecordResult = RecordCommands(Visitor);
                    break;
                }
                default:
                    return std::unexpected(ErrorMessage("Execute: unknown pass type"));
                }
                if (!RecordResult)
                    return std::unexpected(RecordResult.error());
            }
        }

        if (SecBuf) {
            if (auto R = SecBuf->end(); R != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage(Format("Execute: secondary CB end failed: {}", vk::to_string(R))));

            Primary.executeCommands({static_cast<vk::CommandBuffer>(**SecBuf)});
        }

        // Present is explicit: BlitToSwapchain transfer passes already recorded their blits as pass commands
        // above; nothing implicit here.

        // Primary CB was already prepared in BeginFrame. Presentation overlays follow all pass-local output
        // blits and preserve the final swapchain contents.
        if (Result.CmdList.ImGuiPresentationOverlay) {
            if (auto R = RecordImGuiPresentationOverlay(Primary, *Result.CmdList.ImGuiPresentationOverlay); !R)
                return std::unexpected(R.error());
        }
        m_ResourceContext->GetImageTracker().Transition(
            Primary,
            m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex()),
            vk::ImageSubresourceRange{.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                      .baseMipLevel   = 0,
                                      .levelCount     = 1,
                                      .baseArrayLayer = 0,
                                      .layerCount     = 1},
            VulkanImageState{
                .stage  = vk::PipelineStageFlagBits2::eTransfer,
                .layout = vk::ImageLayout::ePresentSrcKHR,
            });
        return {};
    }

    [[nodiscard]] auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> override {
        auto& FC      = m_FrameContext[m_CurrentFrame];
        auto& Primary = FC.PrimaryBuffer;
        // End primary

        if (auto R = Primary.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Format("EndFrame: primary CB end failed: {}", vk::to_string(R))));

        vk::CommandBufferSubmitInfo PrimarySubmitInfo{.commandBuffer = *Primary};

        vk::SemaphoreSubmitInfo PresentCompleteSema{
            .semaphore = *m_FrameContext[m_CurrentFrame].PresentComplete,
            .stageMask = vk::PipelineStageFlagBits2::eTransfer,
        };
        vk::SemaphoreSubmitInfo RenderingCompleteSema{
            .semaphore = m_Swapchain.GetCurrentRenderCompleteSemaphore(),
            .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
        };

        auto TimelineSignalSema =
            m_ResourceContext->GetTimeline().GetSignalSubmitInfo(vk::PipelineStageFlagBits2::eColorAttachmentOutput);

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

        auto PresentRes = m_Swapchain.Present(m_ResourceContext->GetGraphicsQueue());
        if (PresentRes == vk::Result::eErrorOutOfDateKHR || PresentRes == vk::Result::eSuboptimalKHR) {
            if (auto R = m_Swapchain.Recreate(); !R)
                return std::unexpected(R.error().Append("VulkanSwapchain recreation failed after Present error"));
        } else if (PresentRes != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format("Present failed: {}", vk::to_string(PresentRes))));
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % m_FramesInFlight;
        return RHIFrameCompletion{.Value = TimelineSignalSema.value};
    }

    [[nodiscard]] auto WaitFinish(const RHIFrameCompletion& Completion) -> std::expected<void, ErrorMessage> override {
        // This is a read-only host wait on an immutable timeline value. It does not touch command buffers or
        // frame ownership, so RenderThread may call it while RHIThread performs normal submission work.
        if (Completion.Value == 0)
            return {};
        m_ResourceContext->GetTimeline().Wait(Completion.Value);
        return {};
    }
    // RAII resources

    uint32_t m_FramesInFlight = 2;
    uint32_t m_CurrentFrame   = 0;

    // VulkanSwapchain & sync

    VulkanSwapchain             m_Swapchain;
    UPtr<VulkanResourceContext> m_ResourceContext   = nullptr;
    VulkanDescriptorManager     m_DescriptorManager = {};

    std::vector<VulkanFrameContext>    m_FrameContext;
    /// @brief The shared frame-start upload staging (buffer and image lanes).
    ///
    /// Both resource managers borrow it. Destroyed ahead of the resource context's allocator in Shutdown().
    VulkanStagingBelt                  m_StagingBelt                  = {};
    /// @brief All buffer-flavor concerns (creation dispatch, uploads, transient belts).
    ///
    /// The device never moves after creation, so frame-affined tasks safely capture the member's address.
    VulkanBufferManager                m_BufferManager                = {};
    /// @brief All texture concerns (creation dispatch, upload front-end).
    VulkanTextureManager               m_TextureManager               = {};
    /// @brief All acceleration-structure concerns (creation, frame-start build batch, AS rings).
    VulkanAccelerationStructureManager m_AccelerationStructureManager = {};
    bool                               m_NeedsShutdown                = false;
};

} // namespace SoulEngine
