add_repositories("souvenal-repo git@github.com:Souvenal/xmake-repo.git")

local vulkan_header_configs = {
    modules = true,
    -- USE_STD_EXPECTED is enabled by default in c++23
    cxx23 = true,
    -- assert on result won't give a good error message;
    -- define it to a no-op so we handle errors via std::expected ourselves.
    assert_on_result = "(void)(x)",
    -- beta extensions like `VK_KHR_portability_subset` are required for MoltenVK on macOS,
    -- so enable them by default.
    enable_beta_extensions = true,
    -- allow vulkan_hpp to use `DispatchLoaderDynamic`,
    -- which uses `vkGetInstanceProcAddr` to load all vulkan APIs at runtime,
    -- rather than static dispatching, by calling the symbols exported by the library.
    dispatch_loader_dynamic = true,
    -- ban vulkan_hpp from throwing exceptions
    no_exceptions = true,
    -- enable c++20 designate initializer syntax for aggregate type
    no_constructors = true
}

add_requires("vulkan-headers", {
    configs = vulkan_header_configs
})

add_requireconfs('vulkan-memory-allocator.vulkan-headers', {
    configs = vulkan_header_configs
})
add_requires("vulkan-memory-allocator v3.4.0")

-- TODO: pack vulkan-loader on macOS
-- add_requires("vulkan-loader 1.4.350+0")

-- TODO: set slangc=false
--       weird, setting it to false will case compilation failure
add_requires("slang[embed_stdlib_source=false] vulkan-sdk-1.4.350.0")

-- assimp's CMakeLists defaults ASSIMP_BUILD_USE_CCACHE=ON, which puts ccache
-- between cmcldeps and rc.exe under the Ninja generator and breaks the
-- cl-based RC dependency scan on Windows. The use_ccache config is provided
-- by souvenal-repo's assimp recipe.
add_requires("assimp", {configs = {use_ccache = false}})
