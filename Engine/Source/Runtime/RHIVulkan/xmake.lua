target("VMA")
    set_kind("static")

    add_packages("vulkan-memory-allocator", "vulkan-headers")
    add_files("VMA.cpp")

target("RHIVulkan")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    -- Routes Tracy's Vulkan calls through a dispatcher-populated symbol table
    -- (see VKProfiling.cppm) instead of extern loader entry points, keeping
    -- vulkan-1.lib out of the link — the engine loads Vulkan dynamically.
    add_defines("TRACY_VK_USE_SYMBOL_TABLE")

    add_packages("volk", {public = true})
    add_packages("tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw", "magic_enum", "imgui", "imgui-club", "hlslpp", {public = true})
    add_deps("RHI", "VMA", "WindowSystem", "TaskGraph")
    add_files("*.cppm", {public = true})   -- RHIVulkan primary module + partitions

target("RHIVulkan.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("RHIVulkan", "Core.Impl", "RHI.Impl", "WindowSystem.Impl", "TaskGraph.Impl", "VMA")
    add_defines("TRACY_VK_USE_SYMBOL_TABLE")
    add_packages("volk", "tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw", "magic_enum", "imgui", "imgui-club", "hlslpp")
    -- VMA.cpp belongs solely to target VMA; compiling it here would duplicate VMA_IMPLEMENTATION.
    add_files("*.cpp|VMA.cpp")             -- module implementation units
