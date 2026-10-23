target("STBImage")
    set_kind("static")

    add_packages("stb")
    add_files("STBImage.cpp")

soul_module("Material", {
    deps     = {"Core", "RHI", "STBImage"},
    packages = {"assimp", "entt", "hlslpp", "stb", "magic_enum"},
    impl = {
        deps     = {"Core", "RHI", "STBImage"},
        packages = {"assimp", "entt", "hlslpp", "stb", "magic_enum"},
        -- Keep the STBImage implementation with its dedicated target.
        excludes = {"STBImage.cpp"},
    },
    tests = {},
})
