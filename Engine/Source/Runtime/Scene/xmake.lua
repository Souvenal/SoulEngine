target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp", "entt", "assimp", "magic_enum", {public = true})
    add_deps("Core", "Material", "RHI", {public = true})

    add_files("*.cppm")



-- NOTE: SceneDocument.cpp crashes the clang 23.1.1 optimizer (frontend signal,
-- SEGV 0xC0000005) in instrumentation passes -- memprof-remove-attributes at
-- -O2/-O3 and ee-instrument at -O0, independent of this project's code (crash
-- reproducer: %TEMP%/SceneDocument-757a3d.*). Exclude it until the toolchain
-- is upgraded past 23.1.1; every other test target compiles and links clean.
test_module("Scene", {
    additional_packages = {"entt"},
    exclude_tests = is_config("toolchain", "clang", "llvm") and {"SceneDocument"} or nil,
})
