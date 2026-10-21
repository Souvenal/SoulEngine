target("Scene")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_packages("hlslpp", "entt", "assimp", "magic_enum", {public = true})
    add_deps("Core", "Material", "RHI", {public = true})

    add_files("*.cppm", {public = true})

target("Scene.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("Scene", "Core.Impl", "Material.Impl", "RHI.Impl")
    add_packages("hlslpp", "entt", "assimp", "magic_enum")
    add_files("*.cpp")



-- NOTE: SceneDocument.cpp crashes the clang 23.1.1 optimizer (frontend signal,
-- SEGV 0xC0000005) in instrumentation passes -- memprof-remove-attributes at
-- -O2/-O3 and ee-instrument at -O0, independent of this project's code (crash
-- reproducer: %TEMP%/SceneDocument-757a3d.*). Exclude it until the toolchain
-- is upgraded past 23.1.1; every other test target compiles and links clean.
-- The exclusion is applied per target inside test_module (exclude_on_clang):
-- the configured toolchain name is not visible at script scope under xmake
-- v3 (get_config("toolchain") returns nil there), so the frontend check runs
-- in on_config where target:toolchains() is resolved.
test_module("Scene", {
    additional_deps = {"Scene.Impl"},
    additional_packages = {"entt"},
    exclude_on_clang = {"SceneDocument"},
})
