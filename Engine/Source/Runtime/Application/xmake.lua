target("Application")
    set_kind("moduleonly")

    add_deps("Core", "Renderer", "Scene", "Resource", "Window")

    add_files("Application.cppm")
    add_files("Applications/*.cppm")
