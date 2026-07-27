module;

export module WindowSystem;

import Core;

export import :Types;
export import :Interface;
export import :Glfw;
export import std;

export namespace SoulEngine {

/// @brief Create the platform default window system (currently GLFW).
///
/// The facade owns implementation selection so Launch and tests never name a
/// concrete backend; future Cocoa/WinUI implementations slot in here.
[[nodiscard]] inline auto CreateWindowSystem() -> std::expected<UPtr<IWindowSystem>, ErrorMessage> {
    auto Result = GlfwWindowSystem::Create();
    if (!Result)
        return std::unexpected(std::move(Result).error());
    return std::make_unique<GlfwWindowSystem>(std::move(*Result));
}

} // namespace SoulEngine
