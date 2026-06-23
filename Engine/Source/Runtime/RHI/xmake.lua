target("RHI")
    set_kind("moduleonly")

    add_deps("Core", "Shader")
    add_packages("glfw")

    add_files("*.cppm")      -- RHI primary module + partitions

target("VMA")
    set_kind("static")

    add_packages("vulkan-memory-allocator", "vulkan-headers")
    add_files("Vulkan/VMA.cpp")

target("RHIVulkan")
    set_kind("moduleonly")

    add_packages("tracy", "vulkan-headers", "vulkan-memory-allocator", "glfw")
    add_deps("RHI", "VMA")
    add_files("Vulkan/*.cppm")
