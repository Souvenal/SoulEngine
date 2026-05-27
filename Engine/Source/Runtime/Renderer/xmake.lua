target("Renderer")
    set_kind("moduleonly")

    add_packages("glfw")
    add_packages("glm")
    add_deps("Core", "RHI", "RHIVulkan", "Shader", "ShaderCache", "Scene")

    add_files("*.cppm")
