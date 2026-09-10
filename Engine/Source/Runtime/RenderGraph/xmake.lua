target("RenderGraph")
    set_kind("moduleonly")

    add_deps("Core", "RHI", "ShaderCompiler", "TaskGraph")
    -- entt is already a public Core package; restate for Clarity of type_hash keys.
    add_packages("magic_enum", "entt", {public = true})

    add_files("*.cppm")

test_module("RenderGraph", {additional_deps = {"RHI", "RHIVulkan", "TaskGraph"}})
