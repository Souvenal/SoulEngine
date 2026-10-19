target("Shader")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core")
    add_packages("magic_enum")

    add_files("*.cppm", {public = true})
