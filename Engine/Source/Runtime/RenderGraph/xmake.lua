target("RenderGraph")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core", "RHI", "ShaderCompiler", "TaskGraph")
    -- entt is already a public Core package; restate for Clarity of type_hash keys.
    add_packages("magic_enum", "entt", {public = true})

    add_files("*.cppm", {public = true})
    add_files("*.cpp")

test_module("RenderGraph", {additional_deps = {"RHI", "RHIVulkan", "TaskGraph"}})
