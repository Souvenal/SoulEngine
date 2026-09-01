target("Resource")
    set_kind("moduleonly")

    add_deps("Core", "RHI", "ShaderCompiler", "TaskGraph")
    add_files("*.cppm")

test_module("Resource", {
    additional_deps = {"TaskGraph"},
})
