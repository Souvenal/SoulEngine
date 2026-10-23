module;

#include <imgui_threaded_rendering.h>

export module RHI.RenderDevice;

import Core;
import WindowSystem;
import std;
import RHI.Types;
import RHI.Ref;
import RHI.Pipeline;
import RHI.Pass;
import RHI.Command;

namespace SoulEngine {

/// @brief Per-frame RHI packet produced by a renderer, consumed by Execute().
///
/// Owned by RHI so the frame contract is stable: new per-frame payload lands here instead of growing the
/// Execute signature. Field types must be RHI module or RHI-hosted third-party types. FrameSlot retention
/// keeps every referenced payload alive until GPU completion.
export struct RenderResult {
    RenderPassList           CmdList       = {};
    /// Editor ImGui draw data for this frame, consumed on the RHI thread.
    UPtr<ImDrawDataSnapshot> ImGuiSnapshot = nullptr;
};

export enum class RHIBackendType {
    Unknown = 0,
    Vulkan,
};

/// @brief Backend-independent completion point for one submitted frame.
///
/// The token is returned by EndFrame() and stored by the caller alongside the submitted frame packet. A zero
/// value denotes a packet that has not been submitted yet and therefore requires no wait.
export struct RHIFrameCompletion {
    Uint64 Value = 0;
};

export class RHIRenderDevice {
  public:
    RHIRenderDevice()                                          = default;
    RHIRenderDevice(const RHIRenderDevice&)                    = delete;
    auto operator=(const RHIRenderDevice&) -> RHIRenderDevice& = delete;
    RHIRenderDevice(RHIRenderDevice&&)                         = delete;
    auto operator=(RHIRenderDevice&&) -> RHIRenderDevice&      = delete;

    virtual ~RHIRenderDevice() = default;

    /// @brief Initialize backend-native GPU device and presentation state.
    [[nodiscard]] virtual auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage>;

    /// @brief Return the concrete RHI backend tag for integration dispatch.
    [[nodiscard]] virtual auto GetBackendType() const -> RHIBackendType;

    // @brief Buffer creation
    [[nodiscard]] virtual auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage>;

    /// @brief Create a GPU→CPU readback object (a consumer object, not a resource).
    ///
    /// Synchronous pure creation on the calling thread (same contract as CreateBuffer): Ready at return.
    /// SizeBytes is the per-frame payload capacity; the backend derives the ring depth from frames in flight.
    [[nodiscard]] virtual auto CreateReadback(StringView Name, Uint64 SizeBytes)
        -> std::expected<RHIRef<RHIReadback>, ErrorMessage>;

    /// @brief Upload a buffer's full contents, callable from any thread.
    ///
    /// The bytes are snapshotted at the call site and become visible to GPU consumers in the issuing frame —
    /// all of one frame's uploads land together. Data.size() must equal the buffer's SizeBytes.
    [[nodiscard]] virtual auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage>;

    /// @brief Create a texture: sampled and render-target flows converge here.
    ///
    /// Synchronous pure creation on the calling thread (same contract as CreateBuffer): the ref publishes
    /// Ready at return, and texel bytes flow through UploadTexture.
    [[nodiscard]] virtual auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage>;

    /// @brief Queue registration of sampled textures in the device-wide bindless table.
    ///
    /// The request copies its references and executes on the RHI thread for the current frame. The completed
    /// descriptor index becomes observable through FindBindlessTextureIndex after the task publishes it.
    [[nodiscard]] virtual auto RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
        -> std::expected<void, ErrorMessage>;

    /// @brief Find a completed device-wide bindless index for a texture.
    [[nodiscard]] virtual auto FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const
        -> std::optional<Uint32>;

    /// @brief Upload a texture's full mip-0 texels, callable from any thread. Same call-site snapshot
    /// and same-frame visibility contract as UploadBuffer. Data.size() must equal Width × Height × Channels.
    [[nodiscard]] virtual auto UploadTexture(RHIRef<RHITexture> Texture, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage>;

    [[nodiscard]] virtual auto CreateSampler(StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage>;

    [[nodiscard]] virtual auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage>;

    [[nodiscard]] virtual auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage>;

    [[nodiscard]] virtual auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage>;

    [[nodiscard]] virtual auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage>;

    [[nodiscard]] virtual auto
    CreateBottomLevelAccelerationStructure(StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage>;

    [[nodiscard]] virtual auto CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                                   const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage>;

    /// @brief Build a BLAS, callable from any thread.
    ///
    /// The call enqueues a frame-affined task carrying the ref; the build is recorded in the frame-start AS
    /// batch of the issuing frame, after the upload batch, so same-frame consumers are safe by submission
    /// order. The ref must be Ready at the call site.
    [[nodiscard]] virtual auto BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas)
        -> std::expected<void, ErrorMessage>;

    /// @brief Build a per-frame TLAS, callable from any thread. Same frame-affined task
    /// contract as BuildBlas; the build is recorded after the BLAS batch, ahead of every pass.
    [[nodiscard]] virtual auto BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas)
        -> std::expected<void, ErrorMessage>;

    /// @brief Frame-begin hook on the RHI thread. Called once per frame.
    [[nodiscard]] virtual auto Tick() -> std::expected<void, ErrorMessage>;
    // RHI frame lifecycle and command execution

    /// @brief Prepare the current backend frame slot before frame-affined tasks run.
    [[nodiscard]] virtual auto BeginFrame() -> std::expected<void, ErrorMessage>;

    /// @brief Execute one frame packet borrowed from the caller.
    ///
    /// Records the frame's AS work first — the pending BLAS batch, then each TLAS build — followed by the
    /// pass list, after BeginFrame() has completed. Does not take ownership of Result; the caller keeps the
    /// packet alive until WaitFinish() confirms its submission is no longer GPU visible.
    [[nodiscard]] virtual auto Execute(RenderResult& Result) -> std::expected<void, ErrorMessage>;

    /// @brief Finish and submit the current backend frame slot.
    ///
    /// Returns the backend-independent completion point that identifies this submission. The caller must
    /// retain the associated command packet until WaitFinish() succeeds for the returned token.
    [[nodiscard]] virtual auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage>;

    /// @brief Block until the GPU has completed the identified submission.
    ///
    /// This host wait is safe for the render thread and does not submit work or mutate the command packet. A
    /// zero-valued token returns immediately.
    [[nodiscard]] virtual auto WaitFinish(const RHIFrameCompletion& Completion) -> std::expected<void, ErrorMessage>;
    // RHICommand context access

    /// @brief Return the current frame-in-flight index.
    [[nodiscard]] virtual auto GetCurrentFrameIndex() const -> Uint32;

    /// @brief Block the CPU until all GPU work completes.
    /// Safe to call at any point after Init(); required before destroying GPU resources that may still be
    /// referenced by in-flight commands.
    [[nodiscard]] virtual auto WaitIdle() -> std::expected<void, ErrorMessage>;
    // Shutdown

    /// @brief Graceful teardown before destruction.
    /// Must be called before the object is destroyed.
    [[nodiscard]] virtual auto Shutdown() -> std::expected<void, ErrorMessage>;
    // Singleton lifecycle

    /// @brief Create and initialize the process-wide RHI singleton.
    ///
    /// Reads the `[Render].RHI` field from the engine config to determine which GPU backend to bootstrap.
    /// Must be called exactly once during engine initialization, before any other RHI access.
    ///
    /// @return Error on failure (e.g., backend not found).
    [[nodiscard]] static auto Create(StringView Backend, IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage>;

    /// @brief Destroy the process-wide RHI singleton.
    ///
    /// Calls Shutdown() on the backend instance, then releases ownership. Safe to call multiple times. After
    /// the first call the singleton is destroyed and subsequent Get() calls are invalid.
    [[nodiscard]] static auto Destroy() -> std::expected<void, ErrorMessage>;

    /// @brief Access the process-wide RHI singleton.
    ///
    /// Valid only between a successful Create() and Destroy().
    [[nodiscard]] static auto Get() -> RHIRenderDevice&;

  private:
    [[nodiscard]] static auto NextTransientBufferId() -> Uint64;

    static UPtr<RHIRenderDevice> s_Instance;
};

// RHIBackendFactory — defined after RHIRenderDevice so the type is complete

/// @brief Factory type for RHI backend creation.
///
/// Each backend (Vulkan, Metal, D3D12, …) auto-registers via AutoRegistrar in its own standalone module —
/// zero changes needed here to add a new backend.
export using RHIBackendFactory = Factory<RHIRenderDevice>;

} // namespace SoulEngine
