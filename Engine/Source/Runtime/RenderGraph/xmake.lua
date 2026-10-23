soul_module("RenderGraph", {
    deps = {"Core", "RHI", "ShaderCompiler", "TaskGraph"},
    -- entt is already a public Core package; restate for Clarity of type_hash keys.
    packages = {"magic_enum", "entt"},
    impl = {
        deps     = {"Core", "RHI", "ShaderCompiler", "TaskGraph"},
        packages = {"magic_enum", "entt"},
    },
    tests = {additional_deps = {"RHI", "RHIVulkan", "TaskGraph"}},
})
