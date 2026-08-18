module;

#include <stb_image.h>

export module Resource:Texture;

export import Core;
export import RHI;
import TaskGraph;
export import std;

export namespace SoulEngine {

/// Request descriptor for sampled texture creation.
struct SampledTextureRequest {
    StringView TexturePath = {};
};

struct DecodedTexture {
    std::vector<Uint8> Pixels;
    Uint32             Width  = 0;
    Uint32             Height = 0;
};

[[nodiscard]] auto NormalizeResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

[[nodiscard]] auto DecodeTexture(StringView TexturePath) -> std::expected<DecodedTexture, ErrorMessage> {
    int W  = 0;
    int H  = 0;
    int Ch = 0;

    auto* Pixels = stbi_load(String(TexturePath).c_str(), &W, &H, &Ch, 4);
    if (!Pixels) {
        const char* Reason = stbi_failure_reason();
        return std::unexpected(ErrorMessage(
            Format("stbi_load failed for '{}': {}", TexturePath, Reason ? StringView(Reason) : StringView("unknown"))));
    }

    const std::size_t  PixelByteCount = static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4;
    std::vector<Uint8> PixelBuffer(Pixels, Pixels + PixelByteCount);
    stbi_image_free(Pixels);

    return DecodedTexture{
        .Pixels = std::move(PixelBuffer),
        .Width  = static_cast<Uint32>(W),
        .Height = static_cast<Uint32>(H),
    };
}

[[nodiscard]] auto RequestSampledTexture(const SampledTextureRequest& Req)
    -> std::expected<RHIRef<RHISampledTexture>, ErrorMessage> {
    const auto Path = NormalizeResourcePath(Req.TexturePath);
    
    auto Decoded = DecodeTexture(Path);
    if (!Decoded) {
        return std::unexpected(Decoded.error());
    }

    auto Created = RHIRenderDevice::Get().CreateSampledTexture(Path, RHISampledTextureDesc{
        .Data     = std::as_bytes(std::span{Decoded->Pixels}),
        .Width    = Decoded->Width,
        .Height   = Decoded->Height,
        .Channels = 4,
        .Format   = RHIFormat::R8G8B8A8_UNORM,
        .Usage    = RHITextureUsage::ShaderResource,
    });
    if (!Created) {
        return std::unexpected(Created.error());
    }

    return *Created;
}

} // namespace SoulEngine
