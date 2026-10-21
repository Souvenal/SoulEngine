soul_module("Renderer", {
    deps     = {"Core", "EditorTypes", "Material", "RHI", "RHIVulkan", "Scene", "RenderGraph"},
    packages = {"magic_enum", "glfw", "hlslpp"},
    impl = {
        deps     = {"Core", "Material", "RHI", "RHIVulkan", "Scene", "RenderGraph"},
        packages = {"magic_enum", "glfw", "hlslpp"},
    },
    tests = {additional_packages = {"entt", "glfw"}},
})
