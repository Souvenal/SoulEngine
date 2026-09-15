module;

export module Renderer:GBuffer;

import Core;
import RHI;
import RenderGraph;

export import std;

export namespace SoulEngine {

/// @brief Raster G-buffer layout and one view's per-frame graph handles.
///
/// Renderer-internal concept (ADR 05): the targets are pooled graph
/// transients created fresh on the builder each frame; this struct only
/// carries the layout constants and that frame's handles. RayTracing does
/// not produce a GBuffer.
struct GBuffer {
    static constexpr Uint32                                      ColorAttachmentCount = 4;
    static constexpr RHIFormat                                   DepthFormat          = RHIFormat::D32_SFLOAT;
    static constexpr std::array<RHIFormat, ColorAttachmentCount> ColorFormats         = {
        RHIFormat::B8G8R8A8_UNORM,
        RHIFormat::R16G16B16A16_SFLOAT,
        RHIFormat::R32_UINT,
        RHIFormat::R32_UINT,
    };
    /// Picking sentinel: the EntityId texel value meaning "no entity".
    static constexpr Uint32 BackgroundEntityId = std::numeric_limits<Uint32>::max();

    RGTextureHandle Albedo     = {};
    RGTextureHandle Normal     = {};
    RGTextureHandle MaterialId = {};
    RGTextureHandle EntityId   = {};
    RGTextureHandle Depth      = {};
};

/// @brief One view's raster view targets: the GBuffer plus SceneColor.
struct RasterViewTargets {
    GBuffer         GBuffer    = {};
    RGTextureHandle SceneColor = {};

    /// @brief Create one view's raster targets on the frame's builder.
    ///
    /// Descriptors carry only extent + format; usage bits are derived by Compile
    /// from the passes' views (ADR 05).
    [[nodiscard]] static auto Create(RenderGraphBuilder& Graph,
                                     StringView          ViewName,
                                     Uint32              Width,
                                     Uint32              Height) -> RasterViewTargets {
        const auto Tex = [&](StringView Suffix, RHIFormat Fmt) {
            return Graph.CreateTexture(Format("{}/{}", ViewName, Suffix),
                                       RGTextureDesc{.Width = Width, .Height = Height, .Format = Fmt});
        };
        return RasterViewTargets{
            .GBuffer =
                SoulEngine::GBuffer{
                    .Albedo     = Tex("GBuffer/Albedo", SoulEngine::GBuffer::ColorFormats[0]),
                    .Normal     = Tex("GBuffer/Normal", SoulEngine::GBuffer::ColorFormats[1]),
                    .MaterialId = Tex("GBuffer/MaterialId", SoulEngine::GBuffer::ColorFormats[2]),
                    .EntityId   = Tex("GBuffer/EntityId", SoulEngine::GBuffer::ColorFormats[3]),
                    .Depth      = Tex("GBuffer/Depth", SoulEngine::GBuffer::DepthFormat),
                },
            .SceneColor = Tex("SceneColor", RHIFormat::B8G8R8A8_UNORM),
        };
    }
};

} // namespace SoulEngine
