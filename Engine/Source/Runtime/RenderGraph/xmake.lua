target("RenderGraph")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core", "RHI", "ShaderCompiler", "TaskGraph")
    -- entt is already a public Core package; restate for Clarity of type_hash keys.
    add_packages("magic_enum", "entt", {public = true})

    add_files("*.cppm", {public = true})

target("RenderGraph.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("RenderGraph", "Core.Impl", "RHI.Impl", "ShaderCompiler.Impl", "TaskGraph.Impl")
    add_packages("magic_enum", "entt")
    add_files("*.cpp")

test_module("RenderGraph", {additional_deps = {"RenderGraph.Impl", "RHI", "RHIVulkan", "TaskGraph"}})
