target("Shader")
    set_kind("moduleonly")

    add_deps("Core")
    add_packages("magic_enum")

    add_files("*.cppm")
