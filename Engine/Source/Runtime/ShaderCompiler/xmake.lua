target("ShaderCompiler")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    -- c++ module bmi is needed by downstream target
    add_packages("slang", {public = true})
    add_packages("magic_enum", {public = true})
    add_deps("Core", "Shader")

    add_files("**.cppm", {public = true})

target("ShaderCompiler.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("ShaderCompiler", "Core.Impl")
    add_packages("slang", "magic_enum")
    add_files("**.cpp|Tests/**")

test_module("ShaderCompiler", {
    deps = {"ShaderCompiler", "ShaderCompiler.Impl", "Core", "Core.Impl", "Shader"},
    packages = {"slang"},
    rpath_packages = {"slang"}
})
