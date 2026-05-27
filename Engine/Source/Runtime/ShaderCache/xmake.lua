target("ShaderCache")
    set_kind("moduleonly")

    add_deps("Core", "Shader", "ShaderCompiler")

    add_files("*.cppm")

test_module("ShaderCache", {
    deps = {"ShaderCache", "Core", "ShaderCompiler"}
})
