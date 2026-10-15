module;

#include <imgui_threaded_rendering.h>

export module RHI:RenderDevice;

import Core;
import WindowSystem;
import std;
import :Types;
import :Ref;
import :Pipeline;
import :Pass;
import :Command;

namespace SoulEngine {

/// Error for backend methods a concrete device leaves unimplemented. Tests run a base RHIRenderDevice (or a
/// small override) to keep RenderGraph's realize step off the real GPU; reaching this default there is
/// expected, in production it means a backend missed an override.
[[nodiscard]] auto NotImplemented(StringView Method) -> ErrorMessage {
    auto Message = ErrorMessage(Format("RHIRenderDevice: '{}' is not implemented by this device", Method));
    LogError("{}", Message.ToString());
    return Message;
}

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Per-frame RHI packet produced by a renderer, consumed by Execute().
///
/// Owned by RHI so the frame contract is stable: new per-frame payload lands here instead of growing the
/// Execute signature. Field types must be RHI module or RHI-hosted third-party types. FrameSlot retention
/// keeps every referenced payload alive until GPU completion.
struct RenderResult {
    RenderPassList           CmdList       = {};
    /// Editor ImGui draw data for this frame, consumed on the RHI thread.
    UPtr<ImDrawDataSnapshot> ImGuiSnapshot = nullptr;
};

enum class RHIBackendType {
    Unknown = 0,
    Vulkan,
};

/// @brief Backend-independent completion point for one submitted frame.
///
/// The token is returned by EndFrame() and stored by the caller alongside the submitted frame packet. A zero
/// value denotes a packet that has not been submitted yet and therefore requires no wait.
struct RHIFrameCompletion {
    Uint64 Value = 0;
};

class RHIRenderDevice {
  public:
    RHIRenderDevice()                                          = default;
    RHIRenderDevice(const RHIRenderDevice&)                    = delete;
    auto operator=(const RHIRenderDevice&) -> RHIRenderDevice& = delete;
    RHIRenderDevice(RHIRenderDevice&&)                         = delete;
    auto operator=(RHIRenderDevice&&) -> RHIRenderDevice&      = delete;

    virtual ~RHIRenderDevice() = default;

    /// @brief Initialize backend-native GPU device and presentation state.
    [[nodiscard]] virtual auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("Initialize"));
    }

    /// @brief Return the concrete RHI backend tag for integration dispatch.
    [[nodiscard]] virtual auto GetBackendType() const -> RHIBackendType {
        return RHIBackendType::Unknown;
    }

    // @brief Buffer creation
    [[nodiscard]] virtual auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateBuffer"));
    }

    /// @brief Create a GPU→CPU readback object (a consumer object, not a resource).
    ///
    /// Synchronous pure creation on the calling thread (same contract as CreateBuffer): Ready at return.
    /// SizeBytes is the per-frame payload capacity; the backend derives the ring depth from frames in flight.
    [[nodiscard]] virtual auto CreateReadback(StringView Name, Uint64 SizeBytes)
        -> std::expected<RHIRef<RHIReadback>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateReadback"));
    }

    /// @brief Upload a buffer's full contents, callable from any thread.
    ///
    /// The bytes are snapshotted at the call site and become visible to GPU consumers in the issuing frame —
    /// all of one frame's uploads land together. Data.size() must equal the buffer's SizeBytes.
    [[nodiscard]] virtual auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("UploadBuffer"));
    }

    /// @brief Create a texture: sampled and render-target flows converge here.
    ///
    /// Synchronous pure creation on the calling thread (same contract as CreateBuffer): the ref publishes
    /// Ready at return, and texel bytes flow through UploadTexture.
    [[nodiscard]] virtual auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateTexture"));
    }

    /// @brief Queue registration of sampled textures in the device-wide bindless table.
    ///
    /// The request copies its references and executes on the RHI thread for the current frame. The completed
    /// descriptor index becomes observable through FindBindlessTextureIndex after the task publishes it.
    [[nodiscard]] virtual auto RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
        -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("RegisterBindlessTextures"));
    }

    /// @brief Find a completed device-wide bindless index for a texture.
    [[nodiscard]] virtual auto FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const
        -> std::optional<Uint32> {
        return std::nullopt;
    }

    /// @brief Upload a texture's full mip-0 texels, callable from any thread. Same call-site snapshot
    /// and same-frame visibility contract as UploadBuffer. Data.size() must equal Width × Height × Channels.
    [[nodiscard]] virtual auto UploadTexture(RHIRef<RHITexture> Texture, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("UploadTexture"));
    }

    [[nodiscard]] virtual auto CreateSampler(StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateSampler"));
    }

    [[nodiscard]] virtual auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateShaderBindingSet"));
    }

    [[nodiscard]] virtual auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateGraphicsPipeline"));
    }

    [[nodiscard]] virtual auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateComputePipeline"));
    }

    [[nodiscard]] virtual auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateRayTracingPipeline"));
    }

    [[nodiscard]] virtual auto
    CreateBottomLevelAccelerationStructure(StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateBottomLevelAccelerationStructure"));
    }

    [[nodiscard]] virtual auto CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                                   const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> {
        return std::unexpected(NotImplemented("CreateTopLevelAccelerationStructure"));
    }

    /// @brief Build a BLAS, callable from any thread.
    ///
    /// The call enqueues a frame-affined task carrying the ref; the build is recorded in the frame-start AS
    /// batch of the issuing frame, after the upload batch, so same-frame consumers are safe by submission
    /// order. The ref must be Ready at the call site.
    [[nodiscard]] virtual auto BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas)
        -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("BuildBlas"));
    }

    /// @brief Build a per-frame TLAS, callable from any thread. Same frame-affined task
    /// contract as BuildBlas; the build is recorded after the BLAS batch, ahead of every pass.
    [[nodiscard]] virtual auto BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas)
        -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("BuildTlas"));
    }

    /// @brief Frame-begin hook on the RHI thread. Called once per frame.
    [[nodiscard]] virtual auto Tick() -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("Tick"));
    }
    // RHI frame lifecycle and command execution

    /// @brief Prepare the current backend frame slot before frame-affined tasks run.
    [[nodiscard]] virtual auto BeginFrame() -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("BeginFrame"));
    }

    /// @brief Execute one frame packet borrowed from the caller.
    ///
    /// Records the frame's AS work first — the pending BLAS batch, then each TLAS build — followed by the
    /// pass list, after BeginFrame() has completed. Does not take ownership of Result; the caller keeps the
    /// packet alive until WaitFinish() confirms its submission is no longer GPU visible.
    [[nodiscard]] virtual auto Execute(RenderResult& Result) -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("Execute"));
    }

    /// @brief Finish and submit the current backend frame slot.
    ///
    /// Returns the backend-independent completion point that identifies this submission. The caller must
    /// retain the associated command packet until WaitFinish() succeeds for the returned token.
    [[nodiscard]] virtual auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> {
        return std::unexpected(NotImplemented("EndFrame"));
    }

    /// @brief Block until the GPU has completed the identified submission.
    ///
    /// This host wait is safe for the render thread and does not submit work or mutate the command packet. A
    /// zero-valued token returns immediately.
    [[nodiscard]] virtual auto WaitFinish(const RHIFrameCompletion& Completion) -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("WaitFinish"));
    }
    // RHICommand context access

    /// @brief Return the current frame-in-flight index.
    [[nodiscard]] virtual auto GetCurrentFrameIndex() const -> Uint32 {
        return 0;
    }

    /// @brief Block the CPU until all GPU work completes.
    /// Safe to call at any point after Init(); required before destroying GPU resources that may still be
    /// referenced by in-flight commands.
    [[nodiscard]] virtual auto WaitIdle() -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("WaitIdle"));
    }
    // Shutdown

    /// @brief Graceful teardown before destruction.
    /// Must be called before the object is destroyed.
    [[nodiscard]] virtual auto Shutdown() -> std::expected<void, ErrorMessage> {
        return std::unexpected(NotImplemented("Shutdown"));
    }
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
    [[nodiscard]] static auto NextTransientBufferId() -> Uint64 {
        static std::atomic<Uint64> NextId = 1;
        return NextId.fetch_add(1, std::memory_order_relaxed);
    }

    static UPtr<RHIRenderDevice> s_Instance;
};

// RHIBackendFactory — defined after RHIRenderDevice so the type is complete

/// @brief Factory type for RHI backend creation.
///
/// Each backend (Vulkan, Metal, D3D12, …) auto-registers via AutoRegistrar in its own standalone module —
/// zero changes needed here to add a new backend.
using RHIBackendFactory = Factory<RHIRenderDevice>;

// Static member definitions

inline UPtr<RHIRenderDevice> RHIRenderDevice::s_Instance = nullptr;

[[nodiscard]] inline auto RHIRenderDevice::Create(StringView Backend, IWindowSystem* WindowSys)
    -> std::expected<void, ErrorMessage> {
    LogInfo("Configured RHI backend: '{}'", Backend);

    // Verify the backend is registered before attempting creation.
    if (!RHIBackendFactory::Get().Contains(Backend)) {
        String Supported;
        auto   Names = RHIBackendFactory::Get().Keys();
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (i > 0)
                Supported += ", ";
            Supported += Names[i];
        }
        return std::unexpected(
            ErrorMessage(Format("Unsupported RHI backend: '{}'. Supported backends: {}", Backend, Supported)));
    }

    // Create the backend via self-registering factory — no switch, no concrete backend imports needed.
    auto Ctx = RHIBackendFactory::Get().Create(Backend);
    if (!Ctx)
        return std::unexpected(ErrorMessage(Format("Failed to create '{}' RHI backend", Backend)));
    if (!WindowSys)
        return std::unexpected(ErrorMessage("RHI backend requires a window system"));
    if (auto R = Ctx->Initialize(WindowSys); !R) {
        return std::unexpected(R.error().Append(Format("Failed to initialize '{}' RHI backend", Backend)));
    }

    s_Instance             = std::move(Ctx);
    GDeferredDeletionQueue = &GDeferredDeletionQueueStorage;
    return {};
}

[[nodiscard]] inline auto RHIRenderDevice::Destroy() -> std::expected<void, ErrorMessage> {
    if (!s_Instance)
        return {};

    auto ShutdownResult = s_Instance->Shutdown();
    s_Instance.reset();
    GDeferredDeletionQueue = nullptr;
    if (!ShutdownResult)
        return std::unexpected(ShutdownResult.error());
    return {};
}

inline auto RHIRenderDevice::Get() -> RHIRenderDevice& {
    return *s_Instance;
}

} // namespace SoulEngine
