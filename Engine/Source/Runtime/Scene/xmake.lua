soul_module("Scene", {
    deps     = {"Core", "Material", "RHI"},
    packages = {"hlslpp", "entt", "assimp", "magic_enum"},
    impl = {
        deps     = {"Core", "Material", "RHI"},
        packages = {"hlslpp", "entt", "assimp", "magic_enum"},
    },
    tests = {
        additional_packages = {"entt"},
    },
})
