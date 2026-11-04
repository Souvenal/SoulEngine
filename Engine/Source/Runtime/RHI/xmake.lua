soul_module("RHI", {
    deps     = {"Core", "Shader", "WindowSystem", "TaskGraph"},
    packages = {"magic_enum", "imgui", "imgui-club", "hlslpp"},
    impl = {
        deps     = {"Core", "WindowSystem"},
        packages = {"magic_enum", "imgui", "imgui-club", "hlslpp"},
    },
    tests = {
        additional_deps     = {"WindowSystem"},
        additional_packages = {"glfw", "imgui", "imgui-club", "hlslpp"},
    },
})
