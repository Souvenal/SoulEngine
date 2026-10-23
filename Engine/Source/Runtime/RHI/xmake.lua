soul_module("RHI", {
    deps     = {"Core", "Shader", "WindowSystem", "TaskGraph"},
    packages = {"magic_enum", "imgui", "imgui-club", "hlslpp"},
    impl = {
        deps     = {"Core", "WindowSystem"},
        packages = {"magic_enum", "imgui", "imgui-club", "hlslpp"},
    },
    tests = {
        additional_deps     = {"RHIVulkan", "ShaderCompiler", "WindowSystem"},
        additional_packages = {"glfw", "vulkan-headers", "imgui", "imgui-club", "hlslpp"},
    },
})
