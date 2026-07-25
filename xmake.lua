set_project("SoulEngine")
set_version("0.1.0")

set_languages("c++23")
set_policy("build.c++.modules", true)

-- A normal add_deps edge guarantees the dependent binary is available for
-- linking, but the job graph may still compile source files from both targets
-- concurrently. A named-module import reads the producer IFC during
-- compilation, so a clean build can otherwise start that import before the
-- IFC has been generated. Fence only moduleonly targets: they produce IFCs
-- consumed across targets, while ordinary static targets remain parallel.
rule("soulengine.msvc_module_fence")
    on_config(function(target)
        if target:is_plat("windows") and target:is_moduleonly() then
            target:set("policy", "build.fence", true)
        end
    end)
rule_end()

if is_plat("windows") then
    set_toolchains("msvc")

    if is_mode("debug") then
        set_runtimes("MDd")
        -- mode.debug would enable "debug" symbols by default, but its /Zi
        -- format makes concurrent source compilations write one shared
        -- compile.<target>.pdb and can fail with C1041. "embed" changes that
        -- format to /Z7, embedding each source's debug information in its
        -- own object file; the linker still emits the final binary PDB.
        -- "embed" is a format modifier, so it must be paired with "debug".
        set_symbols("debug", "embed")
    else
        set_runtimes("MD")
    end
else
    set_toolchains("clang")
    add_cxxflags("-fno-exceptions", "-fno-rtti")
end

add_rules("mode.release", "mode.releasedbg", "mode.debug")
add_rules("soulengine.msvc_module_fence")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

-- Use right-handed coordinates (OpenGL/Vulkan convention) for hlslpp.
-- Affects look_at, perspective, rotation functions — all assume z points
-- backward (away from viewer).  Default is left-handed (DirectX/Metal).
add_defines("HLSLPP_COORDINATES=1")

set_targetdir("$(projectdir)/Engine/Binaries")
set_installdir("$(projectdir)/Engine/Binaries")

includes("Engine")
