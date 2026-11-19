soul_module("WindowSystem", {
    deps     = {"Core", "EditorCommands"},
    packages = {"entt", "glfw", "imgui", "magic_enum", "nlohmann_json"},
    impl = {
        deps     = {"Core", "EditorCommands"},
        packages = {"entt", "glfw", "imgui", "magic_enum", "nlohmann_json"},
    },
    tests = {},
})
