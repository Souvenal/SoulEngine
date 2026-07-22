target("Renderer")
    set_kind("moduleonly")

    add_packages("glfw", "hlslpp")
    add_deps("Core", "Material", "RHI", "RHIVulkan", "Scene", "Resource")

    add_files("*.cppm")

test_module("Renderer", {additional_packages = {"entt", "glfw"}})
