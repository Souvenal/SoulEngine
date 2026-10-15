module;

export module RenderGraph:Types;

import Core;
import RHI;

import std;

export namespace SoulEngine {

inline constexpr Uint32 kRGInvalidIndex = 0xFFFFFFFF;

/// @brief RenderGraph texture identity: a passive index into the graph's resource table.
///
/// Copying is cheap and the default value is invalid. The graph owns the resource through its table; the
/// identity only indexes it. Lifetime flavor (transient belt, pooled target, readback import) lives on the
/// resource entry, so identities of every flavor share this one shape.
struct RGTexture {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGTexture&) const noexcept -> bool = default;
};

/// @brief RenderGraph buffer identity: a passive index into the graph's resource table.
///
/// Same contract as RGTexture; the two kinds mirror the RHI resource convergence.
struct RGBuffer {
    Uint32 Index = kRGInvalidIndex;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return Index != kRGInvalidIndex; }
    [[nodiscard]] auto operator==(const RGBuffer&) const noexcept -> bool = default;
};

/// @brief Type-erased texture view record: identity, role, extent, access, and the graph-resolved resource.
///
/// The graph's erased machinery manipulates views through this one non-template type. Pass Parameter fields
/// use the RGTextureView template below; every specialization derives from this record.
struct RGTextureViewData {
    RGTexture           Texture  = {};
    RHITextureUsage     Usage    = RHITextureUsage::None;
    RHISubresourceRange Range    = {};
    RHIAccess           Access   = {};
    RHIRef<RHITexture>  Resolved = {};  ///< Graph-resolved resource; valid from pass execution time on.

    /// @brief The ready RHI view over Resolved, built from the declared usage, range, and access.
    [[nodiscard]] auto ResolvedView() const -> RHITextureView {
        return {.Texture = Resolved, .Access = Access, .Usage = Usage, .Range = Range};
    }
};

/// @brief A pass's texture declaration: role (usage bits) and access ride the template arguments; the wiring
/// site assigns the resource identity only.
///
/// The pass alone knows how it consumes the resource. With role and access in the field type, the Parameter
/// stays an aggregate and the wiring site initializes each field by name with the identity alone
/// (`{.Output = Target}`); assigning a whole record to a field is ill-formed, so the declared role cannot
/// be reset. At Compile the graph fills `.Resolved` with the realized (or imported) resource; pass execution
/// builds the ready RHI view from the declared usage, range, and access on demand via ResolvedView().
template <RHITextureUsage U, RHIAccess A>
struct RGTextureView : RGTextureViewData {
    // Base members take no declaration-site defaults from here, so the declared role lands in the ctor body.
    RGTextureView() { Usage = U; Access = A; }
    RGTextureView(RGTexture In) : RGTextureView() { Texture = In; }
};

/// @brief Type-erased buffer view record: identity, role, access, and the graph-resolved resource.
///
/// Same contract as RGTextureViewData. Pass Parameter fields use the RGBufferView template below; every
/// specialization derives from this record.
struct RGBufferViewData {
    RGBuffer          Buffer   = {};
    RHIBufferUsage    Usage    = RHIBufferUsage::Unknown;
    RHIAccess         Access   = {};
    RHIRef<RHIBuffer> Resolved = {};  ///< Graph-resolved resource; valid from pass execution time on.

    /// @brief The ready RHI view over Resolved, built from the declared usage and access.
    [[nodiscard]] auto ResolvedView() const -> RHIBufferView {
        return {.Buffer = Resolved, .Access = Access, .Usage = Usage};
    }
};

/// @brief A pass's buffer declaration: role (usage bits) and access ride the template arguments; the wiring
/// site assigns the resource identity only.
///
/// Same contract as RGTextureView.
template <RHIBufferUsage U, RHIAccess A>
struct RGBufferView : RGBufferViewData {
    // Base members take no declaration-site defaults from here, so the declared role lands in the ctor body.
    RGBufferView() { Usage = U; Access = A; }
    RGBufferView(RGBuffer In) : RGBufferView() { Buffer = In; }
};

/// @brief One registered pass.
///
/// The graph stores only what ordering, pruning, and pipeline binding need; the concrete pass type hides
/// behind the IRHIPass base. Accesses ride the two view pointer lists (one per resource kind); direction
/// lives on each view's Access field, so consumers (ordering, pruning, usage check, prologue) walk exactly
/// the lists they need.
struct RGPassNode {
    String Name = {};

    // Pass-type static markers, read from TPass at AddPass.
    bool NeverPrune    = false;  ///< TPass::NeverPrune — pruning must never drop this pass
    bool HasPipeline   = false;  ///< TPass declares BuildPipelineRequest()

    /// @brief The pass, constructed at registration.
    ///
    /// Every pointer in `Buffers`/`Textures` leads into this object's Parameter. Pruning drops the whole
    /// node; Compile moves the survivors into the frame list.
    UPtr<IRHIPass> Pass = nullptr;

    /// @brief The pass's buffer views: pointers into the pass's Parameter.
    std::vector<RGBufferViewData*>  Buffers  = {};
    /// @brief The pass's texture views: pointers into the pass's Parameter.
    std::vector<RGTextureViewData*> Textures = {};

    /// @brief Coarse pipeline class of the pass, detected statically from TPass's RHI pass base at AddPass.
    ///
    /// Combined with Usage and Access it fixes a transition's stage/access without shader reflection.
    RHIPassType StageClass = RHIPassType::Unknown;

    /// @brief Pass indices that must execute before this one (last-writer chain).
    std::vector<Uint32> Dependencies = {};

    /// @brief PipelineRegistry key when HasPipeline (type-hash of TPass).
    ///
    /// The graph is type-erased after AddPass, so readiness queries go through this key.
    String PipelineKey = {};
};

/// @brief Buffer-kind payload of a resource entry.
///
/// `Desc` and `OwnedInitialData` carry graph-created state that realize consumes at Compile; imported entries
/// hold `Resolved` alone.
struct RGBufferProp {
    RHIBufferDesc          Desc = {};  ///< creation desc; graph-created entries only
    /// @brief Graph-owned copy of CreateBuffer's InitialData.
    ///
    /// Callers may hand in spans over temporaries that die before Compile (realize forwards the bytes to the
    /// RHI), so the graph copies them at registration.
    std::vector<std::byte> OwnedInitialData = {};
    RHIRef<RHIBuffer>      Resolved = nullptr;  ///< imported at AddPass; realized at Compile
};

/// @brief Texture-kind payload of a resource entry.
///
/// `Desc` is the creation declaration a graph-created texture was registered with; the pool keys reuse and
/// return on it verbatim. Imported entries hold `Resolved` alone.
struct RGTextureProp {
    RHITextureDesc     Desc     = {};
    /// @brief True once Compile acquired the texture from the pool.
    ///
    /// The builder destructor keys the pool return on this, not on ref readiness — a just-created ref may
    /// still be pending (or a null mock handle) when the builder dies.
    bool               FromPool = false;
    RHIRef<RHITexture> Resolved = nullptr;  ///< imported at AddPass; acquired from the pool at Compile
};

/// @brief One registered resource.
///
/// Either imported (the caller owns the RHI object; the graph only reads the ref) or graph-created (realized
/// lazily during Compile, only if a surviving pass touches it). Kind lives in `Prop`; the constructors are
/// the only way in, so an entry is born complete.
struct RGResourceEntry {
    // Imported entries hold the ref alone; the owner carries everything else.
    RGResourceEntry(String Name, RHIRef<RHITexture> Imported)
        : Name(std::move(Name)), IsImported(true), Prop(RGTextureProp{.Resolved = std::move(Imported)}) {}
    RGResourceEntry(String Name, RHIRef<RHIBuffer> Imported)
        : Name(std::move(Name)), IsImported(true), Prop(RGBufferProp{.Resolved = std::move(Imported)}) {}
    // Graph-created entries carry the creation desc; realize consumes it at Compile.
    RGResourceEntry(String Name, RHITextureDesc Desc)
        : Name(std::move(Name)), Prop(RGTextureProp{.Desc = Desc}) {}
    RGResourceEntry(String Name, RHIBufferDesc Desc, std::vector<std::byte> OwnedInitialData)
        : Name(std::move(Name)), Prop(RGBufferProp{.Desc = Desc, .OwnedInitialData = std::move(OwnedInitialData)}) {}

    /// @brief The texture payload when graph-created; nullptr for imported entries and buffers.
    [[nodiscard]] auto AsGraphTexture() -> RGTextureProp* {
        return IsImported ? nullptr : std::get_if<RGTextureProp>(&Prop);
    }
    /// @brief The texture payload when graph-created; nullptr for imported entries and buffers.
    [[nodiscard]] auto AsGraphTexture() const -> const RGTextureProp* {
        return IsImported ? nullptr : std::get_if<RGTextureProp>(&Prop);
    }
    /// @brief The buffer payload when graph-created; nullptr for imported entries and textures.
    [[nodiscard]] auto AsGraphBuffer() -> RGBufferProp* {
        return IsImported ? nullptr : std::get_if<RGBufferProp>(&Prop);
    }
    /// @brief The buffer payload when graph-created; nullptr for imported entries and textures.
    [[nodiscard]] auto AsGraphBuffer() const -> const RGBufferProp* {
        return IsImported ? nullptr : std::get_if<RGBufferProp>(&Prop);
    }

    String Name       = {};
    bool   IsImported = false;

    /// @brief Kind-specific state: buffer or texture, never both.
    std::variant<RGBufferProp, RGTextureProp> Prop = {};

    // Registration-time ordering bookkeeping (last-writer chain).
    Uint32              LastWriter        = kRGInvalidIndex;  ///< pass index of the latest writer
    std::vector<Uint32> ReadersSinceWrite = {};               ///< readers of the latest written state
};

} // namespace SoulEngine
