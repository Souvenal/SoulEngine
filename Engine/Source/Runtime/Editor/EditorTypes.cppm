module;

export module EditorTypes;

import std;
import RHI;
import Scene;

export namespace SoulEngine {

struct RenderPixelCoordinate {
    Uint32 X = 0;
    Uint32 Y = 0;
};

struct ScenePickingRequest {
    RenderPixelCoordinate     Pixel  = {};
    RHIRef<RHIReadbackBuffer> Target = nullptr;
};

struct EditorSnapshot {
    std::vector<CameraViewRecord>      Views   = {};
    std::optional<ScenePickingRequest> Picking = std::nullopt;
};

} // namespace SoulEngine
