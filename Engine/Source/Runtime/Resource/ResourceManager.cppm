module;

#include <stb_image.h>

export module Resource:Manager;

export import Core;
export import RHI;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

/// @brief Central resource manager — singleton.
///
/// Loads assets from disk, uploads to GPU, and caches by path hash.
/// CPU pixel data is released immediately after GPU upload.
class Manager : public Singleton<Manager> {
    friend class Singleton<Manager>;

  public:
    /// @brief Load a texture from disk. Returns cached result if already loaded.
    /// stb loads as RGBA (4 channels). CPU pixels freed after GPU upload.
    [[nodiscard]] auto LoadTexture(StringView Path) -> std::expected<SPtr<RHI::Texture>, ErrorMessage> {
        auto Hash = std::hash<StringView>{}(Path);

        if (auto It = m_TextureCache.find(Hash); It != m_TextureCache.end())
            return It->second;

        // ── stb load ──────────────────────────────────────────────────
        int   W = 0, H = 0, Ch = 0;
        // Force RGBA output regardless of source format
        auto* Pixels = stbi_load(String(Path).c_str(), &W, &H, &Ch, 4);
        if (!Pixels)
            return std::unexpected(ErrorMessage(Format("stbi_load failed for '{}': {}", Path, stbi_failure_reason())));

        // ── Upload to GPU ─────────────────────────────────────────────
        RHI::TextureDesc Desc{
            .Data     = Pixels,
            .Width    = static_cast<Uint32>(W),
            .Height   = static_cast<Uint32>(H),
            .Channels = 4,
            .Format   = RHI::Format::R8G8B8A8_UNORM,
            .Usage    = RHI::TextureUsage::ShaderResource,
        };

        auto TexResult = RHI::RenderDevice::Get().CreateTexture(Desc);

        // CPU data no longer needed — release immediately
        stbi_image_free(Pixels);

        if (!TexResult)
            return std::unexpected(TexResult.error().Append(Format("Failed to create GPU texture for '{}'", Path)));

        auto Tex = std::move(*TexResult);
        m_TextureCache.emplace(Hash, Tex);

        LogInfo("Loaded texture '{}' ({}x{})", Path, W, H);
        return Tex;
    }

    /// @brief Clear all cached resources. Call during shutdown.
    auto Clear() -> void {
        m_TextureCache.clear();
    }

  private:
    Manager()  = default;
    ~Manager() = default;

    std::unordered_map<Uint64, SPtr<RHI::Texture>> m_TextureCache;
};

} // namespace SoulEngine::Resource
