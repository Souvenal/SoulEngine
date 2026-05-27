target("Application")
    set_kind("moduleonly")

    add_deps("Core", "Renderer", "Scene")

    add_files("Application.cppm")
    add_files("Applications/*.cppm")
