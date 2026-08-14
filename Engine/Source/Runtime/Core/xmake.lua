-- Keep spdlog in a conventional translation unit.  MSVC cannot compile a
-- module unit that both includes spdlog's transitive standard-library headers
-- and imports the named std module; Core:Logging uses this private C ABI bridge.
target("CoreLoggingBackend")
    set_kind("static")
    set_languages("c++20")

    add_packages("spdlog")
    add_files("Logging/LoggingBackend.cpp")

target("Core")
    set_kind("moduleonly")

    add_deps("CoreLoggingBackend", {public = true})
    -- Clang's dependency scan compiles Config.cppm in consuming targets. Export its toml++
    -- configuration and header path so that scan/compile observes the same parser API; see TODO.md.
    add_defines("TOML_COMPILER_HAS_EXCEPTIONS=0", {public = true})
    add_packages("toml++", "hlslpp", {public = true})

    add_files("**.cppm")

test_module("Core", {
    deps = {"Core"}
})
