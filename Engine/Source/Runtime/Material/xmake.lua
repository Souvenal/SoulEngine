target("STBImage")
    set_kind("static")

    add_packages("stb")
    add_files("STBImage.cpp")

target("Material")
    set_kind("moduleonly")

    add_packages("assimp", "entt", "hlslpp", "stb", "magic_enum", {public = true})
    add_deps("Core", "RHI", "STBImage", {public = true})

    add_files("*.cppm", {public = true})
    -- Keep the STBImage implementation with its dedicated target.
    add_files("*.cpp|STBImage.cpp")

test_module("Material")
