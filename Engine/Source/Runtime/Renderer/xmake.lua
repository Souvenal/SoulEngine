target("Renderer")
    set_kind("static")
    set_policy("build.c++.modules", true)

    add_packages("magic_enum", {public = true})
    add_packages("glfw", "hlslpp")
    add_deps("Core", "EditorTypes", "Material", "RHI", "RHIVulkan", "Scene", "RenderGraph")

    add_files("**.cppm", {public = true})
    add_files("**.cpp|Tests/**")

test_module("Renderer", {additional_packages = {"entt", "glfw"}})
