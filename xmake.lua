set_project("SoulEngine")
set_version("0.1.0")

set_languages("c++23")
set_policy("build.c++.modules", true)

add_rules("mode.release", "mode.releasedbg", "mode.debug")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

add_cxxflags("-fno-exceptions", "-fno-rtti")

set_targetdir("$(projectdir)/Engine/Binaries")
set_installdir("$(projectdir)/Engine/Binaries")

includes("Engine")
