-- magic_enum/spdlog/hlslpp/entt/libyaml must stay reachable through Core's import
-- chain (exported Core API names them, and Core's module objects reference libyaml
-- symbols). Interface packages are public by convention (docs/standards/xmake.md),
-- which also covers toml++: it appears in CVar.cppm's global module fragment.
soul_module("Core", {
    packages = {"spdlog", "hlslpp", "entt", "libyaml", "magic_enum", "toml++"},
    defines  = {"TOML_COMPILER_HAS_EXCEPTIONS=0"},
    impl = {
        packages = {"spdlog", "hlslpp", "entt", "libyaml", "magic_enum", "toml++"},
        defines  = {"TOML_COMPILER_HAS_EXCEPTIONS=0"},
    },
    tests = {},
})
