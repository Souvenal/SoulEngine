target("Core")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    -- magic_enum is public: Core's exported flag vocabulary (Util/Flags.cppm) names
    -- magic_enum::customize::enum_range in the FlagEnum concept and in operator|, and every importer
    -- instantiates that template, so the package must stay reachable through Core's import chain.
    -- spdlog / hlslpp / entt / libyaml are public too; hlslpp and entt because exported Core API
    -- (Types.cppm glue, meta APIs) references them, and libyaml because Core's own module objects
    -- reference its symbols and xmake does not propagate a private package's links to dependents.
    -- toml++ stays private: header-only, used only in CVar.cppm.
    add_packages("spdlog", "hlslpp", "entt", "libyaml", "magic_enum", {public = true})
    add_defines("TOML_COMPILER_HAS_EXCEPTIONS=0")
    add_packages("toml++")

    add_files("**.cppm", {public = true})
    add_files("**.cpp|Tests/**")

test_module("Core", {
    deps = {"Core"}
})
