module;

#include <entt/entity/entity.hpp>

export module EditorTypes;

import std;
import RHI;
import Scene;
import Core;

namespace SoulEngine {

export struct PixelCoordinate {
    Uint32 X = 0;
    Uint32 Y = 0;
};

/// @brief Immutable render data for one editor camera view (no RHI
/// render targets — the view's targets, including the selection mask, are renderer-created pooled graph
/// transients).
export struct EditorViewRecord {
    CameraViewRecord Camera = {};

    /// @brief Return the width of the editor-camera viewport.
    [[nodiscard]] auto GetWidth() const -> Uint32 {
        return Camera.GetWidth();
    }

    /// @brief Return the height of the editor-camera viewport.
    [[nodiscard]] auto GetHeight() const -> Uint32 {
        return Camera.GetHeight();
    }

    /// @brief Convert this editor view to the renderer-neutral camera view.
    [[nodiscard]] auto ToCameraViewRecord() const -> const CameraViewRecord& {
        return Camera;
    }
};

/// @brief Describes the camera's picking readback observation.
///
/// Camera update polls and stores the readback state; BuildSnapshot copies it for post-update collection.
/// The state is read and written on the Game thread while the camera and RHI device remain alive.
export enum class EditorReadbackState { Unknown = 0, Unavailable, LatestKnown, Malformed };

export struct EditorSnapshot {
    std::vector<EditorViewRecord> Views               = {};
    std::optional<CameraViewport> PrimaryViewport     = std::nullopt;
    std::optional<entt::entity>   SelectedEntity      = std::nullopt;
    std::optional<entt::entity>   ReadbackEntity      = std::nullopt;
    PixelCoordinate               HoverPixel          = {};
    bool                          IsHovering          = false;
    bool                          IsReadbackPending   = false;
    bool                          IsReadbackMalformed = false;
    EditorReadbackState           ReadbackState       = EditorReadbackState::Unknown;
    String                        ReadbackReason      = "readback_not_observed";
    RHIRef<RHIReadback>           ReadbackTarget      = nullptr;
};

} // namespace SoulEngine
