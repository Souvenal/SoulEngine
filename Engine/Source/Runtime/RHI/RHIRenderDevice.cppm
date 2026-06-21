module;

#include <glfw/glfw3.h>

export module RHI:RenderDevice;

import std;
import :Types;
import :Command; // CommandList

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

class RenderDevice {
  public:
    RenderDevice()                                       = default;
    RenderDevice(const RenderDevice&)                    = delete;
    auto operator=(const RenderDevice&) -> RenderDevice& = delete;
    RenderDevice(RenderDevice&&)                         = delete;
    auto operator=(RenderDevice&&) -> RenderDevice&      = delete;

    virtual ~RenderDevice() = default;

    /// @brief One-time initialization with the application window.
    /// Must be called exactly once after construction, before any other method.
    /// @param Window  GLFW window handle for surface creation.
    /// @return Error on failure (e.g., no suitable GPU found).
    [[nodiscard]] virtual auto Init(GLFWwindow* Window) -> std::expected<void, ErrorMessage> = 0;

    // ── Resource creation ────────────────────────────────────────────────────

    [[nodiscard]] virtual auto CreateVertexBuffer(const VertexBufferDesc& Desc)
        -> std::expected<SPtr<VertexBuffer>, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateIndexBuffer(const IndexBufferDesc& Desc)
        -> std::expected<SPtr<IndexBuffer>, ErrorMessage> = 0;
    /// Create a constant (uniform) buffer with per-frame backing storage.
    /// The returned buffer auto-selects the current frame's slot on Write().
    /// @param Size  Total size in bytes per-frame slot.
    [[nodiscard]] virtual auto CreateConstantBuffer(const ConstantBufferDesc& Desc)
        -> std::expected<SPtr<ConstantBuffer>, ErrorMessage>                                                  = 0;
    [[nodiscard]] virtual auto CreateTexture(const TextureDesc& Desc) -> std::expected<Texture, ErrorMessage> = 0;
    [[nodiscard]] virtual auto CreateGraphicsPipeline(const GraphicsPipelineDesc& Desc)
        -> std::expected<SPtr<GraphicsPipeline>, ErrorMessage> = 0;

    /// Write data into the engine-global constant buffer bound to Set 0.
    /// Safe to call once per frame after BeginFrame().  Size must not exceed
    /// the global CB capacity (configured via [RHI.Vulkan].GlobalConstantBufferSize).
    [[nodiscard]] virtual auto WriteGlobalConstantBuffer(const void* Data, Uint64 Size)
        -> std::expected<void, ErrorMessage> = 0;

    [[nodiscard]] virtual auto CreateSampler(const SamplerDesc& Desc) -> std::expected<Sampler, ErrorMessage> = 0;

    [[nodiscard]] virtual auto DestroyTexture(Texture TexHdl) -> std::expected<void, ErrorMessage>  = 0;
    [[nodiscard]] virtual auto DestroySampler(Sampler SampHdl) -> std::expected<void, ErrorMessage> = 0;

    // ── Command execution ────────────────────────────────────

    /// @brief Execute a frame's worth of RHI commands.
    /// Replaces the old single-threaded BeginFrame/EndFrame pattern.
    /// The backend handles submission + present internally.
    [[nodiscard]] virtual auto Execute(const CommandList& CmdList) -> std::expected<void, ErrorMessage> = 0;

    // ── Command context access ──────────────────────────────────────────

    /// @brief Return the current frame-in-flight index.
    [[nodiscard]] virtual auto GetCurrentFrameIndex() const -> Uint32 = 0;

    // ── GPU sync ───────────────────────────────────────────────────────────

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
    /// @param Window  GLFW window handle for surface creation.
    /// @return Error on failure (backend not found, init failed, etc.).
    [[nodiscard]] static auto Create(GLFWwindow* Window) -> std::expected<void, ErrorMessage>;

    /// @brief Destroy the process-wide RHI singleton.
    ///
    /// Calls Shutdown() on the backend instance, then releases ownership.
    /// Safe to call multiple times.  After the first call the singleton is
    /// destroyed and subsequent Get() calls are invalid.
    static auto Destroy() -> void;

    /// @brief Access the process-wide RHI singleton.
    ///
    /// Valid only between a successful Create() and Destroy().
    [[nodiscard]] static auto Get() -> RenderDevice&;

  private:
    static UPtr<RenderDevice> s_Instance;
};

// ═════════════════════════════════════════════════════════════════════════════
// BackendFactory — defined after RenderDevice so the type is complete
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Factory type for RHI backend creation.
///
/// Each backend (Vulkan, Metal, D3D12, …) auto-registers via
/// AutoRegistrar in its own standalone module — zero changes needed
/// here to add a new backend.
using BackendFactory = Core::Factory<RenderDevice>;

// ═════════════════════════════════════════════════════════════════════════════
// Static member definitions
// ═════════════════════════════════════════════════════════════════════════════

inline UPtr<RenderDevice> RenderDevice::s_Instance = nullptr;

[[nodiscard]] inline auto RenderDevice::Create(GLFWwindow* Window) -> std::expected<void, ErrorMessage> {
    const auto& Cfg = ConfigManager::Get().GetConfig();

    if (!Cfg.Render.RHI.has_value())
        return std::unexpected(ErrorMessage("RHI backend not configured – set [Render].RHI in the config file"));

    String Backend = Cfg.Render.RHI.value_or("Vulkan");
    LogInfo("Configured RHI backend: '{}'", Backend);

    // Verify the backend is registered before attempting creation.
    if (!BackendFactory::Get().Contains(Backend)) {
        String Supported;
        auto   Names = BackendFactory::Get().Keys();
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (i > 0)
                Supported += ", ";
            Supported += Names[i];
        }
        return std::unexpected(
            ErrorMessage(Core::Format("Unsupported RHI backend: '{}'. Supported backends: {}", Backend, Supported)));
    }

    // Create the backend via self-registering factory — no switch,
    // no concrete backend imports needed.
    auto Ctx = BackendFactory::Get().Create(Backend);

    // Initialize the backend with the application window.
    if (auto R = Ctx->Init(Window); !R)
        return std::unexpected(R.error().Append(Core::Format("Failed to initialize '{}' RHI backend", Backend)));

    s_Instance = std::move(Ctx);
    return {};
}

inline auto RenderDevice::Destroy() -> void {
    if (s_Instance) {
        s_Instance->Shutdown();
        s_Instance.reset();
    }
}

inline auto RenderDevice::Get() -> RenderDevice& {
    return *s_Instance;
}

} // namespace SoulEngine::RHI
