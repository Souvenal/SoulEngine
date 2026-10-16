module;

export module Renderer:GBuffer;

import Core;
import RHI;
import RenderGraph;
import magic_enum;

import std;

export namespace SoulEngine {

/// @brief Raster G-buffer layout and one view's per-frame graph handles.
///
/// Renderer-internal concept: the targets are pooled graph transients created fresh on the builder each
/// frame; this struct only carries that frame's handles. RayTracing does not produce a GBuffer.
struct GBuffer {
    /// Picking sentinel: the EntityId texel value meaning "no entity".
    static constexpr Uint32 BackgroundEntityId = std::numeric_limits<Uint32>::max();

    RGTexture Albedo     = {};
    RGTexture Normal     = {};
    RGTexture MaterialId = {};
    RGTexture EntityId   = {};
    RGTexture Depth      = {};
};

/// @brief One view's raster view targets: the GBuffer plus SceneColor.
struct RasterViewTargets {
    GBuffer   GBuffer    = {};
    RGTexture SceneColor = {};

    /// @brief Create one view's raster targets on the frame's builder.
    ///
    /// Usage unions are declared here at creation: what the targets may be used for is the creator's call;
    /// passes' views are checked against it at Compile.
    [[nodiscard]] static auto Create(RenderGraphBuilder& Graph, StringView ViewName, Uint32 Width, Uint32 Height)
        -> RasterViewTargets {
        const auto Tex = [&](StringView Suffix, RHIFormat Fmt, RHITextureUsage Usage) {
            return Graph.CreateTexture(Format("{}/{}", ViewName, Suffix),
                                       RHITextureDesc{.Width = Width, .Height = Height, .Format = Fmt, .Usage = Usage});
        };
        return RasterViewTargets{
            .GBuffer =
                SoulEngine::GBuffer{
                    .Albedo     = Tex("GBuffer/Albedo",
                                      RHIFormat::B8G8R8A8_UNORM,
                                      RHITextureUsage::RenderTarget | RHITextureUsage::Sampled),
                    .Normal     = Tex("GBuffer/Normal",
                                      RHIFormat::R16G16B16A16_SFLOAT,
                                      RHITextureUsage::RenderTarget | RHITextureUsage::Sampled),
                    .MaterialId = Tex("GBuffer/MaterialId",
                                      RHIFormat::R32_UINT,
                                      RHITextureUsage::RenderTarget | RHITextureUsage::Sampled),
                    .EntityId =
                        Tex("GBuffer/EntityId",
                            RHIFormat::R32_UINT,
                            RHITextureUsage::RenderTarget | RHITextureUsage::Sampled | RHITextureUsage::TransferSrc),
                    .Depth = Tex("GBuffer/Depth",
                                 RHIFormat::D32_SFLOAT,
                                 RHITextureUsage::DepthStencil | RHITextureUsage::Sampled),
                },
            .SceneColor = Tex("SceneColor",
                              RHIFormat::B8G8R8A8_UNORM,
                              RHITextureUsage::RenderTarget | RHITextureUsage::Sampled | RHITextureUsage::TransferSrc),
        };
    }
};

} // namespace SoulEngine
