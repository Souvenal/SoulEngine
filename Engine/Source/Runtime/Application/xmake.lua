target("Application")
    set_kind("moduleonly")

    add_deps("Core", "Renderer", "Scene", "Resource")

    add_files("Application.cppm")
    add_files("Applications/*.cppm")
