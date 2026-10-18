target("Platform")
    set_kind("static")

    set_policy("build.c++.modules", true)

    add_deps("Core")

    add_files("Platform.cppm", {public = true})

    if is_plat("windows") then
        add_files("Windows/**.cpp")
        add_syslinks("dbghelp")
    elseif is_plat("macosx") then
        add_files("Mac/**.cpp")
    end
