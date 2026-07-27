target("WindowSystem")
    set_kind("moduleonly")

    add_packages("glfw", {public = true})
    add_packages("imgui", {public = true})
    add_deps("Core")

    add_files("**.cppm")
