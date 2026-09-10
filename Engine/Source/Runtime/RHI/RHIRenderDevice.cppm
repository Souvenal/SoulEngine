export module RHI:RenderDevice;

export import Core;
export import WindowSystem;
export import std;
import :Types;
import :Ref;
import :Pipeline;
import :RayTracing;
import :Pass;

namespace SoulEngine {
namespace {

/// Error for backend methods a concrete device leaves unimplemented. Tests
/// run a base RHIRenderDevice (or a small override) to keep RenderGraph's
/// realize step off the real GPU; reaching this default there is expected,
/// in production it means a backend missed an override.
[[nodiscard]] auto NotImplemented(StringView Method) -> ErrorMessage {
    auto Message = ErrorMessage(Format("RHIRenderDevice: '{}' is not implemented by this device", Method));
    LogError("{}", Message.ToString());
    return Message;
}

} // namespace
} // namespace SoulEngine

export namespace SoulEngine {

enum class RHIBackendType {
    Unknown = 0,
    Vulkan,
};

/// @brief Backend-independent completion point for one submitted frame.
///
/// The token is returned by EndFrame() and stored by the caller alongside the
/// submitted frame packet.  A zero value denotes a packet that has not been
/// submitted yet and therefore requires no wait.
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
    [[nodiscard]] virtual auto Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> { return std::unexpected(NotImplemented("Initialize")); }

    /// @brief Return the concrete RHI backend tag for integration dispatch.
    [[nodiscard]] virtual auto GetBackendType() const -> RHIBackendType { return RHIBackendType::Unknown; }

    // ── Resource creation ────────────────────────────────────────────────────

    [[nodiscard]] virtual auto CreateVertexBuffer(StringView Name, const RHIVertexBufferDesc& Desc)
        -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> { return std::unexpected(NotImplemented("CreateVertexBuffer")); }

    [[nodiscard]] virtual auto CreateIndexBuffer(StringView Name, const RHIIndexBufferDesc& Desc)
        -> std::expected<RHIRef<RHIIndexBuffer>, ErrorMessage> { return std::unexpected(NotImplemented("CreateIndexBuffer")); }

    [[nodiscard]] virtual auto CreateSampledTexture(StringView Name, const RHISampledTextureDesc& Desc)
        -> std::expected<RHIRef<RHISampledTexture>, ErrorMessage> { return std::unexpected(NotImplemented("CreateSampledTexture")); }

    [[nodiscard]] virtual auto CreateSampler(StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> { return std::unexpected(NotImplemented("CreateSampler")); }

    [[nodiscard]] virtual auto CreateRenderTarget(StringView Name, const RHIRenderTargetDesc& Desc)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> { return std::unexpected(NotImplemented("CreateRenderTarget")); }

    /// @brief Create a persistent host-visible GPU-to-CPU readback buffer.
    [[nodiscard]] virtual auto CreateReadbackBuffer(StringView Name, const RHIReadbackBufferDesc& Desc)
        -> std::expected<RHIRef<RHIReadbackBuffer>, ErrorMessage> { return std::unexpected(NotImplemented("CreateReadbackBuffer")); }

    [[nodiscard]] virtual auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> { return std::unexpected(NotImplemented("CreateShaderBindingSet")); }

    [[nodiscard]] virtual auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> { return std::unexpected(NotImplemented("CreateGraphicsPipeline")); }

    [[nodiscard]] virtual auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> { return std::unexpected(NotImplemented("CreateComputePipeline")); }

    [[nodiscard]] virtual auto CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> { return std::unexpected(NotImplemented("CreateRayTracingPipeline")); }

    [[nodiscard]] virtual auto
    CreateBottomLevelAccelerationStructure(StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> { return std::unexpected(NotImplemented("CreateBottomLevelAccelerationStructure")); }

    [[nodiscard]] virtual auto CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                                   const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> { return std::unexpected(NotImplemented("CreateTopLevelAccelerationStructure")); }

    /// @brief Retire backend-native completion callbacks. Called once per frame on the RHI thread.
    virtual auto Tick() -> void { NotImplemented("Tick"); }

    /// @brief Create a frame-affined transient constant buffer from a data snapshot.
    [[nodiscard]] virtual auto CreateTransientConstantBuffer(StringView                         Name,
                                                             const RHITransientConstantBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientConstantBuffer>, ErrorMessage> { return std::unexpected(NotImplemented("CreateTransientConstantBuffer")); }

    [[nodiscard]] auto CreateTransientConstantBuffer(const RHITransientConstantBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientConstantBuffer>, ErrorMessage> {
        return CreateTransientConstantBuffer("Transient/ConstantBuffer", Desc);
    }

    /// @brief Create a frame-affined transient shader-storage buffer from a data snapshot.
    [[nodiscard]] virtual auto CreateTransientShaderStorageBuffer(
        StringView                              Name,
        const RHITransientShaderStorageBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientShaderStorageBuffer>, ErrorMessage> { return std::unexpected(NotImplemented("CreateTransientShaderStorageBuffer")); }

    [[nodiscard]] auto CreateTransientShaderStorageBuffer(const RHITransientShaderStorageBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientShaderStorageBuffer>, ErrorMessage> {
        return CreateTransientShaderStorageBuffer("Transient/ShaderStorageBuffer", Desc);
    }

    // ── RHI frame lifecycle and command execution ─────────────────────────

    /// @brief Prepare the current backend frame slot before frame-affined tasks run.
    [[nodiscard]] virtual auto BeginFrame() -> std::expected<void, ErrorMessage> { return std::unexpected(NotImplemented("BeginFrame")); }

    /// @brief Execute a frame's worth of RHI commands borrowed from the caller.
    ///
    /// Records the current frame's commands after BeginFrame() has completed,
    /// but does not take ownership of PassList.  The caller keeps the packet
    /// alive until WaitFinish() confirms that its submission is no longer GPU
    /// visible.
    [[nodiscard]] virtual auto Execute(RenderPassList& PassList) -> std::expected<void, ErrorMessage> { return std::unexpected(NotImplemented("Execute")); }

    /// @brief Finish and submit the current backend frame slot.
    ///
    /// Returns the backend-independent completion point that identifies this
    /// submission.  The caller must retain the associated command packet until
    /// WaitFinish() succeeds for the returned token.
    [[nodiscard]] virtual auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> { return std::unexpected(NotImplemented("EndFrame")); }

    /// @brief Block until the GPU has completed the identified submission.
    ///
    /// This host wait is safe for the render thread and does not submit work or
    /// mutate the command packet.  A zero-valued token returns immediately.
    [[nodiscard]] virtual auto WaitFinish(const RHIFrameCompletion& Completion)
        -> std::expected<void, ErrorMessage> { return std::unexpected(NotImplemented("WaitFinish")); }

    // ── RHICommand context access ──────────────────────────────────────────

    /// @brief Return the current frame-in-flight index.
    [[nodiscard]] virtual auto GetCurrentFrameIndex() const -> Uint32 { return 0; }

    /// @brief Block the CPU until all GPU work completes.
    /// Safe to call at any point after Init(); required before destroying
    /// GPU resources that may still be referenced by in-flight commands.
    virtual auto WaitIdle() -> void { NotImplemented("WaitIdle"); }

    // ── Shutdown ─────────────────────────────────────────────────────────

    /// @brief Graceful teardown before destruction.
    /// Must be called before the object is destroyed.
    virtual auto Shutdown() -> void { NotImplemented("Shutdown"); }

    // ── Singleton lifecycle ──────────────────────────────────────────────────

    /// @brief Create and initialize the process-wide RHI singleton.
    ///
    /// Reads the `[Render].RHI` field from the engine config to determine
    /// which GPU backend to bootstrap.  Must be called exactly once during
    /// engine initialization, before any other RHI access.
    ///
    /// @return Error on failure (e.g., backend not found).
    [[nodiscard]] static auto Create(StringView Backend, IWindowSystem* WindowSys)
        -> std::expected<void, ErrorMessage>;

    /// @brief Destroy the process-wide RHI singleton.
    ///
    /// Calls Shutdown() on the backend instance, then releases ownership.
    /// Safe to call multiple times.  After the first call the singleton is
    /// destroyed and subsequent Get() calls are invalid.
    static auto Destroy() -> void;

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

// ═════════════════════════════════════════════════════════════════════════════
// RHIBackendFactory — defined after RHIRenderDevice so the type is complete
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Factory type for RHI backend creation.
///
/// Each backend (Vulkan, Metal, D3D12, …) auto-registers via
/// AutoRegistrar in its own standalone module — zero changes needed
/// here to add a new backend.
using RHIBackendFactory = Factory<RHIRenderDevice>;

// ═════════════════════════════════════════════════════════════════════════════
// Static member definitions
// ═════════════════════════════════════════════════════════════════════════════

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

    // Create the backend via self-registering factory — no switch,
    // no concrete backend imports needed.
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

inline auto RHIRenderDevice::Destroy() -> void {
    if (s_Instance) {
        s_Instance->Shutdown();
        s_Instance.reset();
        GDeferredDeletionQueue = nullptr;
    }
}

inline auto RHIRenderDevice::Get() -> RHIRenderDevice& {
    return *s_Instance;
}

} // namespace SoulEngine
