export module RHI:RenderDevice;

export import WindowSystem;
export import std;
import :Types;
import :RayTracing;
import :Command; // RHICommandList

export namespace SoulEngine {

enum class RHIBackendType {
    Unknown = 0,
    Vulkan,
};

class RHIRenderDevice {
  public:
    RHIRenderDevice()                                       = default;
    RHIRenderDevice(const RHIRenderDevice&)                    = delete;
    auto operator=(const RHIRenderDevice&) -> RHIRenderDevice& = delete;
    RHIRenderDevice(RHIRenderDevice&&)                         = delete;
    auto operator=(RHIRenderDevice&&) -> RHIRenderDevice&      = delete;

    virtual ~RHIRenderDevice() = default;

    /// @brief Initialize backend-native GPU device and presentation state.
    [[nodiscard]] virtual auto Initialize(IWindowSystem* WindowSys)
        -> std::expected<void, ErrorMessage> = 0;

    /// @brief Return the concrete RHI backend tag for integration dispatch.
    [[nodiscard]] virtual auto GetBackendType() const -> RHIBackendType = 0;

    // ── Resource creation ────────────────────────────────────────────────────

    [[nodiscard]] virtual auto CreateVertexBuffer(const RHIVertexBufferDesc& Desc)
        -> std::expected<RHIVertexBufferCreateResult, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateIndexBuffer(const RHIIndexBufferDesc& Desc)
        -> std::expected<RHIIndexBufferCreateResult, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateSampler(const RHISamplerDesc& Desc)
        -> std::expected<UPtr<RHISampler>, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateSampledTexture(const RHISampledTextureDesc& Desc)
        -> std::expected<RHISampledTextureCreateResult, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateRenderTarget(const RHIRenderTargetDesc& Desc)
        -> std::expected<RHIRenderTargetCreateResult, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<UPtr<RHIGraphicsPipeline>, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<UPtr<RHIRayTracingPipeline>, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateBottomLevelAccelerationStructure(const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHIBottomLevelAccelerationStructure>, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateTopLevelAccelerationStructure(const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> = 0;

    /// Return the RHIRenderDevice-owned BDA metadata table, or null when hardware ray tracing is unavailable.
    [[nodiscard]] virtual auto GetRayTracingGeometryTable() -> RHIRayTracingGeometryTable* = 0;
    /// Allocate a logical transient constant-buffer handle.
    /// The handle is written through a command list and resolved by the backend during Execute().
    [[nodiscard]] auto AllocateTransientConstantBuffer(Uint64 Size)
        -> std::expected<RHITransientConstantBuffer, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Transient constant buffer size must be greater than zero"));
        return RHITransientConstantBuffer{NextTransientBufferId(), Size};
    }

    /// Allocate a logical transient shader-storage-buffer handle.
    /// The handle is written through a command list and resolved by the backend during Execute().
    [[nodiscard]] auto AllocateTransientShaderStorageBuffer(Uint64 Size)
        -> std::expected<RHITransientShaderStorageBuffer, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Transient shader storage buffer size must be greater than zero"));
        return RHITransientShaderStorageBuffer{NextTransientBufferId(), Size};
    }

    // ── RHICommand execution ────────────────────────────────────

    /// @brief Execute a frame's worth of RHI commands.
    /// Replaces the old single-threaded BeginFrame/EndFrame pattern.
    /// The backend handles submission + present internally.
    [[nodiscard]] virtual auto Execute(const RHICommandList& CmdList) -> std::expected<void, ErrorMessage> = 0;

    // ── RHICommand context access ──────────────────────────────────────────

    /// @brief Return the current frame-in-flight index.
    [[nodiscard]] virtual auto GetCurrentFrameIndex() const -> Uint32 = 0;

    // ── GPU sync ───────────────────────────────────────────────────────────

    /// @brief Non-blocking query for backend GPU completion tokens.
    [[nodiscard]] virtual auto IsGpuComplete(RHIGpuCompletionToken Token) -> bool = 0;

    /// @brief Block the CPU until all GPU work completes.
    /// Safe to call at any point after Init(); required before destroying
    /// GPU resources that may still be referenced by in-flight commands.
    virtual auto WaitIdle() -> void = 0;

    // ── Shutdown ─────────────────────────────────────────────────────────

    /// @brief Graceful teardown before destruction.
    /// Must be called before the object is destroyed.
    virtual auto Shutdown() -> void = 0;

    // ── Singleton lifecycle ──────────────────────────────────────────────────

    /// @brief Create and initialize the process-wide RHI singleton.
    ///
    /// Reads the `[Render].RHI` field from the engine config to determine
    /// which GPU backend to bootstrap.  Must be called exactly once during
    /// engine initialization, before any other RHI access.
    ///
    /// @return Error on failure (e.g., backend not found).
    [[nodiscard]] static auto Create(IWindowSystem* WindowSys)
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

[[nodiscard]] inline auto RHIRenderDevice::Create(IWindowSystem* WindowSys)
    -> std::expected<void, ErrorMessage> {
    const auto& Cfg = ConfigManager::Get().GetConfig();

    if (!Cfg.Render.RHI.has_value())
        return std::unexpected(ErrorMessage("RHI backend not configured – set [Render].RHI in the config file"));

    String Backend = Cfg.Render.RHI.value_or("Vulkan");
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

    s_Instance = std::move(Ctx);
    return {};
}

inline auto RHIRenderDevice::Destroy() -> void {
    if (s_Instance) {
        s_Instance->Shutdown();
        s_Instance.reset();
    }
}

inline auto RHIRenderDevice::Get() -> RHIRenderDevice& {
    return *s_Instance;
}

} // namespace SoulEngine
