target("Application")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core", "Scene")

    add_files("Application.cppm", {public = true})
    add_files("ApplicationInterface.cppm", {public = true})
    add_files("Applications/*.cppm", {public = true})
    add_files("Application.cpp")
