target("Renderer")
    set_kind("moduleonly")

    add_packages("glfw", "hlslpp")
    add_deps("Core", "RHI", "RHIVulkan", "Scene", "Resource")

    add_files("*.cppm")
