module;

export module RenderGraph:Types;

export import Core;
import RHI;

export import std;

export namespace SoulEngine {

inline constexpr Uint32 kRGInvalidIndex = 0xFFFFFFFF;

// ── Handles ────────────────────────────────────────────────────────────────
// A handle is a passive index into the graph's resource table: cheap to copy,
// invalid by default, no ownership. Handles identify logical resources; how a
// pass touches them is declared by the view types below.

struct RGTextureHandle {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGTextureHandle&) const noexcept -> bool = default;
};

struct RGStorageBufferHandle {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGStorageBufferHandle&) const noexcept -> bool = default;
};

struct RGConstantBufferHandle {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGConstantBufferHandle&) const noexcept -> bool = default;
};

struct RGReadbackHandle {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGReadbackHandle&) const noexcept -> bool = default;
};

// ── Views ──────────────────────────────────────────────────────────────────
// A view is a Parameter field type: the type itself declares how the pass
// accesses the resource (attachment write, shader read, copy endpoint, ...).
// Non-view Parameter fields (samplers, bindless arrays, CPU scalars) are
// passthrough and declare nothing.
//
// The `Ref` payload is the resolved RHI object for the frame: filled at
// AddPass for imported resources and at Compile (after realization) for
// graph-created transients. `RGViewTag` is what the aggregate walk detects.

struct RGColorRT {
    using RGViewTag = void;

    RGTextureHandle         Texture = {};
    bool                    Present = false;  ///< terminal swapchain write
    bool                    Load    = false;  ///< attachment loadOp=Load (read-modify-write)
    RHIRef<RHIRenderTarget> Ref     = nullptr;
};

struct RGDepthRT {
    using RGViewTag = void;

    RGTextureHandle         Texture = {};
    RHIRef<RHIRenderTarget> Ref     = nullptr;
};

struct RGTextureSRV {
    using RGViewTag = void;

    RGTextureHandle         Texture = {};
    RHIRef<RHIRenderTarget> Ref     = nullptr;
};

struct RGStorageBufferSRV {
    using RGViewTag = void;

    RGStorageBufferHandle                   Buffer = {};
    RHIRef<RHITransientShaderStorageBuffer> Ref    = nullptr;
};

struct RGStorageBufferUAV {
    using RGViewTag = void;

    RGStorageBufferHandle                   Buffer = {};
    RHIRef<RHITransientShaderStorageBuffer> Ref    = nullptr;
};

struct RGIndirectBuffer {
    using RGViewTag = void;

    RGStorageBufferHandle                   Buffer = {};
    RHIRef<RHITransientShaderStorageBuffer> Ref    = nullptr;
};

struct RGConstantBufferSRV {
    using RGViewTag = void;

    RGConstantBufferHandle             Buffer = {};
    RHIRef<RHITransientConstantBuffer> Ref    = nullptr;
};

struct RGCopySrc {
    using RGViewTag = void;

    RGTextureHandle         Texture = {};
    RHIRef<RHIRenderTarget> Ref     = nullptr;
};

struct RGCopyDst {
    using RGViewTag = void;

    RGReadbackHandle          Buffer = {};
    RHIRef<RHIReadbackBuffer> Ref    = nullptr;
};

// ── Transient creation descriptors ─────────────────────────────────────────

/// Descriptor for a graph-created (transient) render target.
struct RGTextureDesc {
    Uint32    Width      = 0;
    Uint32    Height     = 0;
    RHIFormat Format     = RHIFormat::Unknown;
    bool      AllowDepth = false;
    /// Reserved — must be 1; the realization path cannot create multi-mip
    /// targets today (RHIRenderTargetDesc has no mip field).
    Uint32    MipLevels  = 1;
};

/// Descriptor for a graph-created transient shader-storage buffer. The graph
/// copies InitialData at registration, so the span may point at a temporary
/// that dies before Compile().
struct RGShaderStorageBufferDesc {
    Uint64                                    SizeBytes   = 0;
    Uint32                                    Stride      = 0;
    RHITransientBufferUsage                   Usage       = RHITransientBufferUsage::ShaderRead;
    std::optional<std::span<const std::byte>> InitialData = std::nullopt;
};

/// Descriptor for a graph-created transient constant buffer. Same InitialData
/// ownership rule as RGShaderStorageBufferDesc.
struct RGConstantBufferDesc {
    Uint64                                    SizeBytes   = 0;
    std::optional<std::span<const std::byte>> InitialData = std::nullopt;
};

} // namespace SoulEngine
