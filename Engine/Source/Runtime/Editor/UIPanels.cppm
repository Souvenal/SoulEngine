module;

export module Editor:UIPanels;

import std;
import Scene;

namespace SoulEngine {

/// Why a scene provider? Because scene may change at runtime, and UI doen't depend on a specific scene.
///
/// @brief Point the debug panels at the active scene's material store.
export auto RegisterAllUI(std::function<const Scene*()> SceneProvider) -> void;

} // namespace SoulEngine
