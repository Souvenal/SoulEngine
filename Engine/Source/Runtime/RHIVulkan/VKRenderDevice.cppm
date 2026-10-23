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

export module RHIVulkan.RenderDevice;

import RHI; // RenderPassList and RHI command types.
import TaskGraph;
import vulkan;
import std;

import RHIVulkan.Swapchain;
import RHIVulkan.SurfaceProvider;
import RHIVulkan.Command;
import RHIVulkan.Types;
import RHIVulkan.Semaphore;
import RHIVulkan.Buffer;
import RHIVulkan.Readback;
import RHIVulkan.Staging;
import RHIVulkan.FrameContext;
import RHIVulkan.Capability;
import RHIVulkan.Descriptor;
import RHIVulkan.ShaderBindingSet;
import RHIVulkan.Pipeline;
import RHIVulkan.RayTracingPipeline;
import RHIVulkan.AccelerationStructure;
import RHIVulkan.Sampler;
import RHIVulkan.Texture;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Profiling;
import Core;
import magic_enum;
import WindowSystem;

namespace SoulEngine {

export class VulkanRenderDevice final : public RHIRenderDevice {
  public:
    VulkanRenderDevice() {}
    ~VulkanRenderDevice() {
        (void)Shutdown();
    }

    [[nodiscard]] auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> override;

    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Vulkan;
    }
    // Frame lifecycle — private

    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override;

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
        -> std::expected<RHIRef<RHIReadback>, ErrorMessage> override;

    /// @brief Upload initial data into a buffer; forwards to the buffer manager.
    ///
    /// Thin forward: upload validation, snapshotting, and the frame-affined task live in VulkanBufferManager
    /// (:Buffer).
    [[nodiscard]] auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        return m_BufferManager.Upload(std::move(Buffer), Data);
    }

    [[nodiscard]] auto CreateSampler(StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> override;

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
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override;

    [[nodiscard]] auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override;

    [[nodiscard]] auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> override;

    [[nodiscard]] auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override;

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

    [[nodiscard]] auto Tick() -> std::expected<void, ErrorMessage> override;

    [[nodiscard]] auto WaitIdle() -> std::expected<void, ErrorMessage> override;

    [[nodiscard]] auto Shutdown() -> std::expected<void, ErrorMessage> override;

  private:
    [[nodiscard]] auto InitializeImGui(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage>;

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
        -> std::expected<void, ErrorMessage>;

    /// @brief Materialize one pass's prologue barriers into the primary command buffer.
    ///
    /// Every dst-only state requirement is diffed against the emission-side trackers, and only state changes
    /// emit a native barrier. Recorded into the primary so each barrier sits between the producer's and this
    /// pass's commands in submission order.
    [[nodiscard]] auto EmitPassPrologue(vk::raii::CommandBuffer& Primary, const RHIBarrierList& Prologue)
        -> std::expected<void, ErrorMessage>;

    /// @brief Record the frame-start upload batch into a dedicated secondary appended ahead of every pass.
    ///
    /// Entries were produced by the frame-affined UploadBuffer/UploadTexture tasks drained earlier this frame
    /// (staging allocation and host copy already done there); the belt records one copy per entry, the buffer
    /// lane sealed by a single aggregated transfer-write → consumer-read barrier. Runs first in the
    /// frame-start phase because the BLAS batch reads geometry buffers. Recorded unconditionally — an empty
    /// batch simply yields an empty secondary.
    [[nodiscard]] auto RecordUploadWork(vk::raii::CommandBuffer& Primary) -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto Execute(RenderResult& Result) -> std::expected<void, ErrorMessage> override;

    [[nodiscard]] auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> override;

    [[nodiscard]] auto WaitFinish(const RHIFrameCompletion& Completion) -> std::expected<void, ErrorMessage> override;
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
