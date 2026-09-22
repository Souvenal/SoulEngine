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
    -- toml++ stays private: only CVar.cppm's global module fragment includes it
    -- (verified 2026-09-22: consumer-target dependency scans no longer need
    -- its header path; see TODO.md).  The define must match that TU only.
    add_defines("TOML_COMPILER_HAS_EXCEPTIONS=0")
    add_packages("hlslpp", "entt", "libyaml", {public = true})
    add_packages("toml++")

    add_files("**.cppm")

test_module("Core", {
    deps = {"Core"}
})
