target("Application")
    set_kind("static")

    add_deps("Core", "Scene")

    add_files("Application.cppm", {public = true})
    add_files("Applications/*.cppm", {public = true})
    add_files("Application.cpp")
