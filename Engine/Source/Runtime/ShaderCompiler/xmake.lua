soul_module("ShaderCompiler", {
    deps     = {"Core", "Shader"},
    -- slang is needed by downstream targets: the C++ module BMI is consumed there.
    packages = {"slang", "magic_enum"},
    impl = {
        deps     = {"Core"},
        packages = {"slang", "magic_enum"},
    },
    tests = {
        deps           = {"Core", "Shader"},
        packages       = {"slang"},
        rpath_packages = {"slang"},
    },
})
