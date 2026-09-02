target("Renderer")
    set_kind("moduleonly")

    add_packages("magic_enum", {public = true})
    add_packages("glfw", "hlslpp")
    add_deps("Core", "EditorTypes", "Material", "RHI", "RHIVulkan", "Scene", "Resource")

    add_files("*.cppm")
    add_files("Editor/*.cppm")
    add_files("Raster/*.cppm")
    add_files("RayTracing/*.cppm")

test_module("Renderer", {additional_packages = {"entt", "glfw"}})
