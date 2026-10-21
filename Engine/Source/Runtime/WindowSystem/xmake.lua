target("WindowSystem")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_packages("entt", "glfw", {public = true})
    add_packages("imgui", {public = true})
    add_packages("magic_enum", {public = true})
    add_deps("Core")

    add_files("**.cppm", {public = true})

target("WindowSystem.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("WindowSystem", "Core.Impl")
    add_packages("entt", "glfw", "imgui", "magic_enum")
    add_files("**.cpp")
