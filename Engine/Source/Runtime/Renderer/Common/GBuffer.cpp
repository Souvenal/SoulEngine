module Renderer:Common.GBuffer.impl;

import :Common.GBuffer;
import Core;
import RHI;
import RenderGraph;

import std;

namespace SoulEngine {

[[nodiscard]] auto
RasterViewTargets::Create(RenderGraphBuilder& Graph, StringView ViewName, Uint32 Width, Uint32 Height)
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
                .Depth = Tex(
                    "GBuffer/Depth", RHIFormat::D32_SFLOAT, RHITextureUsage::DepthStencil | RHITextureUsage::Sampled),
            },
        .SceneColor = Tex("SceneColor",
                          RHIFormat::B8G8R8A8_UNORM,
                          RHITextureUsage::RenderTarget | RHITextureUsage::Sampled | RHITextureUsage::TransferSrc),
    };
}

} // namespace SoulEngine
