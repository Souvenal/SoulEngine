module;

#include <stb_image.h>

export module Resource:Texture;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

export namespace SoulEngine {

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
        return std::unexpected(ErrorMessage(Format(
            "stbi_load failed for '{}': {}", TexturePath, Reason ? StringView(Reason) : StringView("unknown"))));
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

[[nodiscard]] auto SubmitSampledTextureRequest(ResourceContext& Context, StringView TexturePath)
    -> ResourceHandle<RHISampledTexture> {
    const auto Key = NormalizeResourcePath(TexturePath);

    auto Work   = BeginResourceWork<RHISampledTexture>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    LogDebug("Sampled texture requested '{}'", Key);

    auto* ContextPtr = &Context;
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
        [ContextPtr, Generation = Handle.GetGeneration(), Key] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested()) {
            LogDebug("Async texture decode discarded after shutdown '{}'", Key);
            return;
        }

        auto DecodeResult = DecodeTexture(Key);
        if (!DecodeResult) {
            PublishResourceFailed<RHISampledTexture>(Context, Generation, Key, DecodeResult.error());
            return;
        }

        if (Context.IsShutdownRequested()) {
            LogDebug("Async texture upload discarded after shutdown '{}'", Key);
            return;
        }

        auto EnqueueResult = TaskGraph::Get().Enqueue(
            ThreadQueue::RHI,
            [ContextPtr, Generation, Key, Decoded = std::move(*DecodeResult)] {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested()) {
                LogDebug("Async texture publish discarded after shutdown '{}'", Key);
                return;
            }

            if (!MarkResourceRhiCommitting<RHISampledTexture>(Context, Key, Generation))
                return;

            RHISampledTextureDesc Desc{
                .Data     = Decoded.Pixels.data(),
                .Width    = Decoded.Width,
                .Height   = Decoded.Height,
                .Channels = 4,
                .Format   = RHIFormat::R8G8B8A8_UNORM,
                .Usage    = RHITextureUsage::ShaderResource,
            };

            auto TexResult = RHIRenderDevice::Get().CreateSampledTexture(Desc);
            if (!TexResult) {
                PublishResourceFailed<RHISampledTexture>(
                    Context, Generation, Key, TexResult.error().Append(Format("Failed to create GPU texture for '{}'", Key)));
                return;
            }

            PublishResourceGpuPending<RHISampledTexture>(
                Context,
                Generation,
                Key,
                Resource<RHISampledTexture>{.Object = std::move(TexResult->Texture)},
                TexResult->UploadCompletion);
            });
        if (!EnqueueResult) {
            PublishResourceFailed<RHISampledTexture>(
                Context,
                Generation,
                Key,
                EnqueueResult.error().Append(
                    Format("Failed to enqueue async {} work '{}'", ResourceTraits<RHISampledTexture>::Info.Label, Key)));
        }
    });
    if (!EnqueueResult) {
        PublishResourceFailed<RHISampledTexture>(
            Context,
            Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(
                Format("Failed to enqueue async {} work '{}'", ResourceTraits<RHISampledTexture>::Info.Label, Key)));
    }

    return Handle;
}

} // namespace SoulEngine
