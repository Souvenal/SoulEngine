target("Window")
    set_kind("moduleonly")

    add_packages("tracy", "glfw", {public = true})
    add_deps("Core")

    add_files("**.cppm")
