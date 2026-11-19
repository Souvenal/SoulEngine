module;

#include <imgui.h>

export module Editor.State;

import Application;
import Core;
import EditorCommands;
import EditorTypes;
import RHI;
import Scene;
import WindowSystem;
import std;

namespace SoulEngine {

/// @brief Identifies the lifecycle stage carried by a baseline Editor observation.
///
/// Editor advances this stage during initialization, presentation binding, and shutdown; Game reads it.
export enum class EditorLifecycle { Unknown = 0, Initialized, Running, Shutdown };

/// @brief Groups the values sampled for one baseline Editor observation.
///
/// Editor fills this source on Game; the collector reads it synchronously to produce owned JSON.
/// Borrowed values remain valid for the duration of Collect.
export struct EditorStateSource {
    Uint64                        Frame            = 0;
    EditorLifecycle               Lifecycle        = EditorLifecycle::Unknown;
    const Application*            ApplicationValue = nullptr;
    String                        RendererName     = {};
    const Scene*                  SceneValue       = nullptr;
    const IWindowSystem*          Window           = nullptr;
    const ImGuiIO*                IO               = nullptr;
    std::optional<RHIBackendType> RenderBackend    = std::nullopt;
    EditorSnapshot                EditorData       = {};
    std::vector<String>           DesiredPanels    = {};
    std::vector<String>           DrawnPanels      = {};
};

/// @brief Collects bounded baseline Editor observations into owned JSON.
///
/// After camera and UI updates, Collect copies the source into JSON cached for post-update dispatch.
/// Source access and collection occur on the Game thread.
export class EditorStateCollector {
  public:
    [[nodiscard]] auto Collect(const EditorStateSource& Source) const -> CommandJson;
};

} // namespace SoulEngine
