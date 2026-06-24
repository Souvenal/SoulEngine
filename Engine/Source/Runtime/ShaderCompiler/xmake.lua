target("ShaderCompiler")
    set_kind("moduleonly")

    -- c++ module bmi is needed by downstream target
    add_packages("slang", {public = true})
    add_packages("magic_enum")
    add_deps("Core", "Shader")

    add_files("**.cppm")

test_module("ShaderCompiler", {
    deps = {"ShaderCompiler", "Core", "Shader"},
    packages = {"slang"}
})
