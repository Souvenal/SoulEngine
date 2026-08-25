target("Renderer")
    set_kind("moduleonly")

    add_packages("magic_enum", {public = true})
    add_packages("glfw", "hlslpp")
    add_deps("Core", "Material", "RHI", "RHIVulkan", "Scene", "Resource")

    add_files("*.cppm")
    add_files("PostProcess/*.cppm")

test_module("Renderer", {additional_packages = {"entt", "glfw"}})
