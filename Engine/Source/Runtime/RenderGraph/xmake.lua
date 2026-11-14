soul_module("RenderGraph", {
    deps = {"Core", "RHI", "ShaderCompiler", "TaskGraph"},
    packages = {"magic_enum"},
    impl = {
        deps     = {"Core", "RHI", "ShaderCompiler", "TaskGraph"},
        packages = {"magic_enum"},
    },
    tests = {additional_deps = {"RHI", "RHIVulkan", "TaskGraph"}},
})
