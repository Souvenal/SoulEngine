target("Platform")
    set_kind("moduleonly")

    add_deps("Core")

    add_files("Platform.cppm")

    if is_plat("windows") then
        add_files("Windows/**.cppm", "Windows/**.cpp")
        add_syslinks("dbghelp")
    elseif is_plat("macosx") then
        add_files("Mac/**.cppm", "Mac/**.cpp")
    end
