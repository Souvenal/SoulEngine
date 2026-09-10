module;

export module RenderGraph:Pass;

import :Types;
import RHI;

export import std;

export namespace SoulEngine {

/// How a pass touches a resource. The view type of the Parameter field
/// determines the usage; ordering only needs the read/write class and the
/// Present bit, the rest is diagnostic and barrier-hint granularity.
enum class RGUsage : Uint8 {
    Unknown = 0,
    SampledRead,            ///< RGTextureSRV
    ColorAttachmentWrite,   ///< RGColorRT
    DepthAttachmentWrite,   ///< RGDepthRT
    StorageBufferRead,      ///< RGStorageBufferSRV
    StorageBufferWrite,     ///< RGStorageBufferUAV
    IndirectRead,           ///< RGIndirectBuffer
    ConstantBufferRead,     ///< RGConstantBufferSRV
    CopySource,             ///< RGCopySrc
    CopyDestination,        ///< RGCopyDst
};

[[nodiscard]] constexpr auto IsWriteUsage(RGUsage Usage) noexcept -> bool {
    switch (Usage) {
    case RGUsage::ColorAttachmentWrite:
    case RGUsage::DepthAttachmentWrite:
    case RGUsage::StorageBufferWrite:
    case RGUsage::CopyDestination:
        return true;
    default:
        return false;
    }
}

/// One registered pass. The graph stores only what ordering, pruning, and
/// construction need; the concrete pass type is erased into `Build`.
struct RGPassNode {
    String Name = {};

    /// Pass-type static markers, read from TPass at AddPass.
    bool NeverPrune    = false;  ///< TPass::NeverPrune — pruning must never drop this pass
    bool PresentOutput = false;  ///< TPass::PresentOutput — carries the frame Present
    bool HasPipeline   = false;  ///< TPass declares BuildPipelineRequest()

    struct Access {
        Uint32  ResourceIndex = 0;  ///< index into the graph's resource table
        RGUsage Usage         = RGUsage::Unknown;
        bool    Present       = false;  ///< the frame's terminal swapchain write
    };
    std::vector<Access> Accesses = {};

    /// Pass indices that must execute before this one (last-writer chain).
    std::vector<Uint32> Dependencies = {};

    /// PipelineRegistry key when HasPipeline (type-hash of TPass); the graph
    /// is type-erased after AddPass, so readiness queries go through this key.
    String PipelineKey = {};

    /// Resolves the stored Parameter's view payloads against the resource
    /// table (realization is complete by then), then constructs the pass:
    /// pipeline passes receive the registry's Ready pipeline ref, transfer
    /// passes the Parameter alone. Called at most once, in final order, only
    /// for passes that survive pruning.
    std::function<std::expected<UPtr<IRHIPass>, ErrorMessage>()> Build = {};
};

/// One registered resource. Either imported (the caller owns the RHI object;
/// the graph only reads the ref) or graph-created transient (realized lazily
/// during Compile, only if a surviving pass touches it).
struct RGResourceEntry {
    String Name      = {};
    bool   IsImported = false;

    // ── Transient creation state ──
    bool                        IsTransientTexture        = false;
    bool                        IsTransientStorageBuffer  = false;
    bool                        IsTransientConstantBuffer = false;
    RGTextureDesc               TextureDesc               = {};
    RGShaderStorageBufferDesc   StorageBufferDesc         = {};
    RGConstantBufferDesc        ConstantBufferDesc        = {};
    /// Graph-owned copy of the descriptor's InitialData. Callers may hand in
    /// spans over temporaries that die before Compile (realize forwards the
    /// bytes to the RHI), so the graph copies them at registration.
    std::vector<std::byte>      OwnedInitialData          = {};

    // ── Resolved RHI object (imported at AddPass, realized at Compile) ──
    RHIRef<RHIRenderTarget>                  Target          = nullptr;
    RHIRef<RHIReadbackBuffer>                Readback        = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>  StorageBuffer   = nullptr;
    RHIRef<RHITransientConstantBuffer>       ConstantBuffer  = nullptr;

    // ── Registration-time ordering bookkeeping (last-writer chain) ──
    Uint32              LastWriter        = kRGInvalidIndex;  ///< pass index of the latest writer
    std::vector<Uint32> ReadersSinceWrite = {};               ///< readers of the latest written state
};

} // namespace SoulEngine
