target("VMA")
    set_kind("static")

    add_packages("vulkan-memory-allocator", "vulkan-headers")
    add_files("VMA.cpp")

-- TRACY_VK_USE_SYMBOL_TABLE routes Tracy's Vulkan calls through a
-- dispatcher-populated symbol table (see VKProfiling.cppm) instead of extern
-- loader entry points, keeping vulkan-1.lib out of the link — the engine loads
-- Vulkan dynamically. Both targets need it: interface and implementation units
-- compile Tracy headers.
soul_module("RHIVulkan", {
    deps     = {"RHI", "VMA", "WindowSystem", "TaskGraph"},
    packages = {"volk", "tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw", "magic_enum", "imgui", "imgui-club", "hlslpp"},
    defines  = {"TRACY_VK_USE_SYMBOL_TABLE"},
    impl = {
        deps     = {"Core", "RHI", "WindowSystem", "TaskGraph", "VMA"},
        packages = {"volk", "tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw", "magic_enum", "imgui", "imgui-club", "hlslpp"},
        defines  = {"TRACY_VK_USE_SYMBOL_TABLE"},
        -- VMA.cpp belongs solely to target VMA; compiling it here would duplicate VMA_IMPLEMENTATION.
        excludes = {"VMA.cpp"},
    },
})
