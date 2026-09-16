target("RHI")
    set_kind("moduleonly")

    add_deps("Core", "Shader", "WindowSystem", "TaskGraph")
    add_packages("magic_enum", "imgui", "imgui-club", "hlslpp", {public = true})

    add_files("*.cppm")      -- RHI primary module + partitions

target("VMA")
    set_kind("static")

    add_packages("vulkan-memory-allocator", "vulkan-headers")
    add_files("Vulkan/VMA.cpp")

target("RHIVulkan")
    set_kind("moduleonly")

    -- Routes Tracy's Vulkan calls through a dispatcher-populated symbol table
    -- (see VKProfiling.cppm) instead of extern loader entry points, keeping
    -- vulkan-1.lib out of the link — the engine loads Vulkan dynamically.
    add_defines("TRACY_VK_USE_SYMBOL_TABLE")

    add_packages("tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw", "magic_enum", "imgui", "imgui-club", "hlslpp", {public = true})
    add_deps("RHI", "VMA", "WindowSystem", "TaskGraph")
    add_files("Vulkan/*.cppm")

test_module("RHI", {
    additional_deps     = {"RHIVulkan", "ShaderCompiler", "WindowSystem"},
    additional_packages = {"glfw", "vulkan-headers", "imgui", "imgui-club", "hlslpp"},
})
