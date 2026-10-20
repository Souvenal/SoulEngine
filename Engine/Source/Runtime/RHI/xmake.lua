target("RHI")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core", "Shader", "WindowSystem", "TaskGraph")
    add_packages("magic_enum", "imgui", "imgui-club", "hlslpp", {public = true})

    add_files("*.cppm", {public = true})  -- RHI primary module + partitions
    add_files("*.cpp")                    -- module implementation units

test_module("RHI", {
    additional_deps     = {"RHIVulkan", "ShaderCompiler", "WindowSystem"},
    additional_packages = {"glfw", "vulkan-headers", "imgui", "imgui-club", "hlslpp"},
})
