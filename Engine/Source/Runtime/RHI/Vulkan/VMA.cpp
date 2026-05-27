// VMA is a header-only file, thus needs an inplementation
#define VMA_IMPLEMENTATION
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html
// Because we use `DynamicLoader` from vulkan headers, we define:
// no static linking
#define VMA_STATIC_VULKAN_FUNCTIONS 0
// TODO: learn about memory related extensions and enable them
#include <vk_mem_alloc.h>
