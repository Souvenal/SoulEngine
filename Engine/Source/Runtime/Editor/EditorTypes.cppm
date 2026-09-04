module;

#include <entt/entity/entity.hpp>

export module EditorTypes;

import std;
import RHI;
import Scene;

export namespace SoulEngine {

struct PixelCoordinate {
    Uint32 X = 0;
    Uint32 Y = 0;
};

/// @brief Immutable render data and resources for one editor camera view.
struct EditorViewRecord {
    CameraViewRecord        Camera        = {};
    RHIRef<RHIRenderTarget> SelectionMask = nullptr;

    /// @brief Return the width of the editor-camera scene-color target.
    [[nodiscard]] auto GetWidth() const -> Uint32 {
        return Camera.GetWidth();
    }

    /// @brief Return the height of the editor-camera scene-color target.
    [[nodiscard]] auto GetHeight() const -> Uint32 {
        return Camera.GetHeight();
    }

    /// @brief Convert this editor view to the renderer-neutral camera view.
    [[nodiscard]] auto ToCameraViewRecord() const -> const CameraViewRecord& {
        return Camera;
    }
};

struct EditorSnapshot {
    std::vector<EditorViewRecord> Views = {};
    std::optional<entt::entity>         SelectedEntity = std::nullopt;
    PixelCoordinate                     HoverPixel = {};
    bool                                IsHovering = false;
    RHIRef<RHIReadbackBuffer>           ReadbackTarget = nullptr;
};

} // namespace SoulEngine
