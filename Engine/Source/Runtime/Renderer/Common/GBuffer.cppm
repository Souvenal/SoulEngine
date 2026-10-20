export module Renderer:Common.GBuffer;

import Core;
import RenderGraph;

import std;

namespace SoulEngine {

/// @brief Raster G-buffer layout and one view's per-frame graph handles.
///
/// Renderer-internal concept: the targets are pooled graph transients created fresh on the builder each
/// frame; this struct only carries that frame's handles. RayTracing does not produce a GBuffer.
export struct GBuffer {
    /// Picking sentinel: the EntityId texel value meaning "no entity".
    static constexpr Uint32 BackgroundEntityId = std::numeric_limits<Uint32>::max();

    RGTexture Albedo     = {};
    RGTexture Normal     = {};
    RGTexture MaterialId = {};
    RGTexture EntityId   = {};
    RGTexture Depth      = {};
};

/// @brief One view's raster view targets: the GBuffer plus SceneColor.
export struct RasterViewTargets {
    GBuffer   GBuffer    = {};
    RGTexture SceneColor = {};

    /// @brief Create one view's raster targets on the frame's builder.
    ///
    /// Usage unions are declared here at creation: what the targets may be used for is the creator's call;
    /// passes' views are checked against it at Compile.
    [[nodiscard]] static auto Create(RenderGraphBuilder& Graph, StringView ViewName, Uint32 Width, Uint32 Height)
        -> RasterViewTargets;
};

} // namespace SoulEngine
