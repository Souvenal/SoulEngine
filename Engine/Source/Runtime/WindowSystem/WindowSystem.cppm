module;

export module WindowSystem;

import Core;

export import WindowSystem.Types;
export import WindowSystem.Interface;
export import WindowSystem.Glfw;
import std;

namespace SoulEngine {

/// @brief Create the platform default window system (currently GLFW).
///
/// The facade owns implementation selection so Launch and tests never name a concrete backend; future
/// Cocoa/WinUI implementations slot in here.
export [[nodiscard]] inline auto CreateWindowSystem() -> std::expected<UPtr<IWindowSystem>, ErrorMessage> {
    auto Result = GlfwWindowSystem::Create();
    if (!Result)
        return std::unexpected(std::move(Result).error());
    return std::move(*Result);
}

} // namespace SoulEngine
