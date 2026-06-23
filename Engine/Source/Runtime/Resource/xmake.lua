add_requires("stb")

target("STBImage")
    set_kind("static")

    add_packages("stb")
    add_files("STBImage.cpp")

target("Resource")
    set_kind("moduleonly")

    add_packages("stb")
    add_deps("Core", "RHI", "STBImage")
    add_files("*.cppm")
