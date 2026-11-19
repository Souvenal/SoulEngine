module;

#include <GLFW/glfw3.h>
#include <imgui.h>

export module WindowSystem.ImGuiGlfwBackend;

import std;
import Core;
import WindowSystem.Types;

namespace SoulEngine {
namespace ImGuiGlfwAdapter {

/// @brief Initialize the source-matched GLFW platform adapter for Vulkan.
export [[nodiscard]] auto InitializeForVulkan(GLFWwindow* Window) -> std::expected<void, ErrorMessage>;
export auto               NewFrame() -> void;
export auto               Shutdown() -> void;

/// @brief Feed one executed shared input frame to the current ImGui context in logical window coordinates.
export auto ApplyInput(const InputFrame& Frame) -> void;

/// @brief Clear ImGui held input and publish focus loss and cursor exit during owner teardown.
export auto ResetInput() -> void;

} // namespace ImGuiGlfwAdapter
} // namespace SoulEngine
