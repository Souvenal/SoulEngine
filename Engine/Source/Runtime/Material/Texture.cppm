module;

#include <entt/entt.hpp>
#include <stb_image.h>

export module Material:Texture;

export import Core;
export import RHI;
export import std;
import :Types;

export namespace SoulEngine {

/// @brief Loads one external image into an RHI sampled texture.
struct TextureDataLoader {
    using result_type = std::shared_ptr<TextureData>;

    auto operator()(const Path& AssetPath) const -> result_type {
        const auto NormalizedPath = AssetPath.lexically_normal();
        if (NormalizedPath.empty()) {
            LogWarning("TextureDataLoader: texture path is empty");
            return nullptr;
        }

        int Width    = 0;
        int Height   = 0;
        int Channels = 0;
        auto* Pixels = stbi_load(NormalizedPath.string().c_str(), &Width, &Height, &Channels, 4);
        if (!Pixels) {
            const char* Reason = stbi_failure_reason();
            LogWarning("TextureDataLoader: failed to decode '{}': {}",
                       NormalizedPath.string(),
                       Reason ? StringView(Reason) : StringView("unknown error"));
            return nullptr;
        }

        const auto PixelByteCount =
            static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height) * 4;
        std::vector<Uint8> PixelData(Pixels, Pixels + PixelByteCount);
        stbi_image_free(Pixels);

        auto Texture = RHIRenderDevice::Get().CreateSampledTexture(
            NormalizedPath.string(),
            RHISampledTextureDesc{
                .Data     = std::as_bytes(std::span{PixelData}),
                .Width    = static_cast<Uint32>(Width),
                .Height   = static_cast<Uint32>(Height),
                .Channels = 4,
                .Format   = RHIFormat::R8G8B8A8_UNORM,
                .Usage    = RHITextureUsage::ShaderResource,
            });
        if (!Texture) {
            LogWarning("TextureDataLoader: failed to create RHI texture '{}': {}",
                       NormalizedPath.string(),
                       Texture.error().ToString());
            return nullptr;
        }

        return std::make_shared<TextureData>(TextureData{
            .AssetPath = NormalizedPath,
            .Texture   = std::move(*Texture),
        });
    }
};

using TextureDataCache = entt::resource_cache<TextureData, TextureDataLoader>;

} // namespace SoulEngine
