target("Core")
    set_kind("moduleonly")

    -- toml++/spdlog/magic_enum stay private: header-only packages used only in
    -- CVar.cppm / Logging.cppm / Yaml.cppm; nothing to propagate at link time.
    -- libyaml is a compiled library: Core's own module objects reference its
    -- symbols, and xmake does not propagate a private package's links to
    -- dependents, so it must stay public.
    -- hlslpp / entt stay public: exported Core API (Types.cppm glue, meta APIs) references them.
    add_packages("spdlog", "hlslpp", "entt", "libyaml", {public = true})
    add_defines("TOML_COMPILER_HAS_EXCEPTIONS=0")
    add_packages("toml++", "magic_enum")

    add_files("**.cppm")

test_module("Core", {
    deps = {"Core"}
})
