set_project("SoulEngine")
set_version("0.1.0")

set_languages("c++23")
set_policy("build.c++.modules", true)

-- option("clang")
--     set_default(false)
--     set_showmenu("use clang on Windows instead of MSVC (MSVC 14.51 C1116 IFC-merge bug workaround)")
-- option_end()

-- option("vs_clang")
--     set_default(false)
--     set_showmenu("use the LLVM bundled with Visual Studio (avoid standalone-clang CodeGen ICE)")
-- option_end()

-- if is_plat("windows") then
--     -- Opt into clang with `xmake f --clang=y`: MSVC 14.51 (VS 2026) mis-merges
--     -- IFCs when a TU imports several modules that carry std::stop_token
--     -- internal records (fatal error C1116 in <stop_token>).
--     -- `xmake f --vs_clang=y --llvm_sdk=<dir>` selects the LLVM toolchain
--     -- bundled with Visual Studio instead; standalone LLVM clang 22.1.8 has a
--     -- CodeGen ICE compiling Material/MaterialYamlLoader.cppm (entt meta),
--     -- which the VS-bundled 22.1.3 does not hit.
--     if has_config("vs_clang") then
--         set_toolchains("llvm")
--     elseif has_config("clang") then
--         set_toolchains("clang")
--     else
--         set_toolchains("msvc")
--     end

    -- if is_mode("debug") then
    --     -- mode.debug would enable "debug" symbols by default, but its /Zi
        -- format makes concurrent source compilations write one shared
        -- compile.<target>.pdb and can fail with C1041. "embed" changes that
        -- format to /Z7, embedding each source's debug information in its
        -- own object file; the linker still emits the final binary PDB.
        -- "embed" is a format modifier, so it must be paired with "debug".
    --     set_symbols("debug", "embed")
    -- end
-- else
set_toolchains("clang")
add_cxxflags("-fno-exceptions", "-fno-rtti")
-- end

add_rules("mode.release", "mode.releasedbg", "mode.debug")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

-- Use right-handed coordinates (OpenGL/Vulkan convention) for hlslpp.
-- Affects look_at, perspective, rotation functions — all assume z points
-- backward (away from viewer).  Default is left-handed (DirectX/Metal).
add_defines("HLSLPP_COORDINATES=1")

set_targetdir("$(projectdir)/Engine/Binaries")
set_installdir("$(projectdir)/Engine/Binaries")

includes("Engine")
