module;

export module RenderGraph:TexturePool;

import Core;
import RHI;

import std;

namespace SoulEngine {

/// @brief Bucket key of a pooled texture: extent + format + creation-declared usage.
using RGTexturePoolKey = std::tuple<Uint32, Uint32, Uint32, Uint32>;

/// @brief Lift the bucket key from a texture descriptor.
[[nodiscard]] auto RGTexturePoolKeyOf(const RHITextureDesc& Desc) -> RGTexturePoolKey {
    return {Desc.Width, Desc.Height, static_cast<Uint32>(Desc.Format), static_cast<Uint32>(Desc.Usage)};
}

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Idle frames after which a free pooled texture is evicted.
inline constexpr Uint32 kRGEvictIdleFrames = 10;

/// @brief Cross-frame pool of transient textures.
///
/// Key = full descriptor (format + extent + usage); usage is derived by Compile, so buckets never mix
/// incompatible images. Acquire is exclusive within a frame; Release is immediate (no GPU-completion gate)
/// and safe by two invariants: single-queue submission ordering and per-frame image state initialization.
///
/// Threading: Acquire/Release/Tick run on the Render thread (Compile and RenderGraph::Tick); Clear runs from
/// main-thread shutdown after the worker threads join. No locking.
class RGTexturePool {
  public:
    RGTexturePool(const RGTexturePool&)                    = delete;
    auto operator=(const RGTexturePool&) -> RGTexturePool& = delete;
    RGTexturePool(RGTexturePool&&)                         = delete;
    auto operator=(RGTexturePool&&) -> RGTexturePool&      = delete;
    RGTexturePool()                                        = default;

    /// @brief Acquire a texture for the descriptor; creates one on a miss.
    [[nodiscard]] auto Acquire(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> {
        const auto Key = RGTexturePoolKeyOf(Desc);
        auto&      Free = m_Free[Key];
        // step 1: reuse — pop the most recently returned entry. Native creation is async, so a ref released
        // while pending can turn Failed inside the free list; drop those and fall through to re-create.
        while (!Free.empty()) {
            auto Ref = std::move(Free.back().Ref);
            Free.pop_back();
            if (Ref.GetState() != RHIRefState::Failed)
                return Ref;
        }
        // step 2: miss — create through the RHI device
        return RHIRenderDevice::Get().CreateTexture(Name, Desc);
    }

    /// @brief Return a texture to the free list, stamped with the current frame. A just-created ref may
    /// still be pending here (native creation is queued); Failed refs are dropped instead of pooled.
    auto Release(RHIRef<RHITexture> Ref, const RHITextureDesc& Desc) -> void {
        if (!m_Accepting || Ref.GetState() == RHIRefState::Failed)
            return;
        m_Free[RGTexturePoolKeyOf(Desc)].push_back({.Ref = std::move(Ref), .LastUsedFrame = m_CurrentFrame});
    }

    /// @brief Advance the frame counter and evict entries idle for kRGEvictIdleFrames. Called once per
    /// frame before IRenderer::Render().
    auto Tick() -> void {
        ++m_CurrentFrame;
        for (auto& [Key, Entries] : m_Free) {
            std::erase_if(Entries, [&](const Entry& E) {
                return m_CurrentFrame - E.LastUsedFrame >= kRGEvictIdleFrames;
            });
        }
    }

    /// @brief Release every pooled ref. Test reset and Init use this; the pool stays usable afterwards.
    auto Clear() -> void {
        m_Free.clear();
    }

    /// @brief Final shutdown: clear and stop accepting returns. Guards the
    /// RenderGraphBuilder-destructor return path against running after the deferred-deletion queue is gone
    /// (root shutdown-boundary constraint).
    auto Shutdown() -> void {
        m_Free.clear();
        m_Accepting = false;
    }

    /// @brief Re-arm after Init (idempotent).
    auto Activate() -> void {
        m_Accepting = true;
    }

    /// @brief Total free-list entries across all buckets (test introspection).
    [[nodiscard]] auto GetFreeEntryCount() const -> Uint32 {
        Uint32 Count = 0;
        for (const auto& [Key, Entries] : m_Free)
            Count += static_cast<Uint32>(Entries.size());
        return Count;
    }

  private:
    struct Entry {
        RHIRef<RHITexture> Ref;
        Uint64                  LastUsedFrame = 0;
    };

    std::map<RGTexturePoolKey, std::vector<Entry>> m_Free         = {};
    Uint64                            m_CurrentFrame = 0;
    bool                              m_Accepting    = true;
};

} // namespace SoulEngine
