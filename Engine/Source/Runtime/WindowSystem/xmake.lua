target("WindowSystem")
    set_kind("moduleonly")

    add_packages("entt", "glfw", {public = true})
    add_packages("imgui", {public = true})
    add_packages("magic_enum", {public = true})
    add_deps("Core")

    add_files("**.cppm")
