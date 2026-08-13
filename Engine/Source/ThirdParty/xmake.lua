-- Set general default configuration
-- See https://xmake.io/api/description/global-interfaces.html#add-requireconfs
-- add_requireconfs("*", {configs = {shared = true}})

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

-- Force transitive vulkan-headers dependencies onto the same configs,
-- otherwise xmake installs a second `vulkan-headers#1` variant with the
-- recipe defaults alongside ours.
add_requireconfs('vulkan-memory-allocator.vulkan-headers', {
    configs = vulkan_header_configs
})
add_requireconfs("imgui.vulkan-headers", {
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

-- system = false is required because add_tests() creates a test wrapper
-- that resolves packages through its own independent path, which does
-- not inherit the parent add_requires configs.  When gtest is found as
-- a system package (Homebrew), the {main = true} config is silently
-- dropped and the test binary links only -lgtest, missing gtest_main.
-- Forcing a remote build via xrepo works around this.
--
-- TODO(Souvenal): produce a minimal reproduction case and file an issue
-- against xmake-io/xmake.
add_requires("gtest[main]", {system = false})

add_requires("magic_enum[modules]")

-- TODO: use module build
add_requires("toml++")

add_requires("tracy 0.13.0")

add_requires("spdlog[std_format,noexcept]")

-- can't find <vulkan/vulkan.h> when glfw itself is being compiled from source
-- {configs = {shared = true, glfw_include = "vulkan"}}
-- glfw is very small, and suitable for static linking.
add_requires("glfw")

add_requires("hlslpp")

-- Scene Document authoring format and runtime ECS.
add_requires("libyaml 0.2.5")
add_requires("entt")

local imgui_config = {
    glfw = true,
    vulkan = true,
    vulkan_no_proto = true,
}
add_requires("imgui master", {configs = imgui_config})
add_requires("imgui-club")
add_requireconfs("imgui-club.imgui", {configs = imgui_config})
