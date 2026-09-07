target("Renderer")
    set_kind("moduleonly")

    add_packages("magic_enum", {public = true})
    add_packages("glfw", "hlslpp")
    add_deps("Core", "EditorTypes", "Material", "RHI", "RHIVulkan", "Scene", "Resource")

    add_files("Common.cppm", "IRenderer.cppm", "RasterRenderer.cppm", "Renderer.cppm")
    add_files("Editor/*.cppm")
    add_files("Raster/*.cppm")
    -- add_files("RayTracing/*.cppm")  -- Disabled: RayTracing pipeline deprecated
    -- add_files("PostProcess/*.cppm")  -- Reserved for future bloom/tonemap passes

test_module("Renderer", {additional_packages = {"entt", "glfw"}, exclude_tests = {"RayTracingTransform"}})
