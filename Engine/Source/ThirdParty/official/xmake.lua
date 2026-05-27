
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

-- TODO: official module build is broken, try fix it
add_requires('glm[cxx_standard=20]')
