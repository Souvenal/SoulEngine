target("Application")
    set_kind("moduleonly")

    add_deps("Core", "Scene")

    add_files("Application.cppm")
    add_files("Applications/*.cppm")
