module RenderGraph.TexturePool;

import Core;
import RHI;
import std;

namespace SoulEngine {

[[nodiscard]] auto RGTexturePoolKeyOf(const RHITextureDesc& Desc) -> RGTexturePoolKey {
    return {Desc.Width, Desc.Height, static_cast<Uint32>(Desc.Format), static_cast<Uint32>(Desc.Usage)};
}

[[nodiscard]] auto RGTexturePool::Acquire(StringView Name, const RHITextureDesc& Desc)
    -> std::expected<RHIRef<RHITexture>, ErrorMessage> {
    const auto Key  = RGTexturePoolKeyOf(Desc);
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

auto RGTexturePool::Release(RHIRef<RHITexture> Ref, const RHITextureDesc& Desc) -> void {
    if (!m_Accepting || Ref.GetState() == RHIRefState::Failed)
        return;
    m_Free[RGTexturePoolKeyOf(Desc)].push_back({.Ref = std::move(Ref), .LastUsedFrame = m_CurrentFrame});
}

auto RGTexturePool::Tick() -> void {
    ++m_CurrentFrame;
    for (auto& [Key, Entries] : m_Free) {
        std::erase_if(Entries, [&](const Entry& E) { return m_CurrentFrame - E.LastUsedFrame >= kRGEvictIdleFrames; });
    }
}

auto RGTexturePool::Clear() -> void {
    m_Free.clear();
}

auto RGTexturePool::Shutdown() -> void {
    m_Free.clear();
    m_Accepting = false;
}

auto RGTexturePool::Activate() -> void {
    m_Accepting = true;
}

[[nodiscard]] auto RGTexturePool::GetFreeEntryCount() const -> Uint32 {
    Uint32 Count = 0;
    for (const auto& [Key, Entries] : m_Free)
        Count += static_cast<Uint32>(Entries.size());
    return Count;
}

} // namespace SoulEngine
