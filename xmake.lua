set_project("SoulEngine")
set_version("0.1.0")

set_languages("c++23")
set_policy("build.c++.modules", true)

if is_plat("windows") then
    set_toolchains("msvc")

    if is_mode("debug") then
        -- mode.debug would enable "debug" symbols by default, but its /Zi
        -- format makes concurrent source compilations write one shared
        -- compile.<target>.pdb and can fail with C1041. "embed" changes that
        -- format to /Z7, embedding each source's debug information in its
        -- own object file; the linker still emits the final binary PDB.
        -- "embed" is a format modifier, so it must be paired with "debug".
        set_symbols("debug", "embed")
    end
else
    set_toolchains("clang")
    add_cxxflags("-fno-exceptions", "-fno-rtti")
end

add_rules("mode.release", "mode.releasedbg", "mode.debug")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

-- Use right-handed coordinates (OpenGL/Vulkan convention) for hlslpp.
-- Affects look_at, perspective, rotation functions — all assume z points
-- backward (away from viewer).  Default is left-handed (DirectX/Metal).
add_defines("HLSLPP_COORDINATES=1")

set_targetdir("$(projectdir)/Engine/Binaries")
set_installdir("$(projectdir)/Engine/Binaries")

includes("Engine")
