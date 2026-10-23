soul_module("Platform", {
    deps = {"Core"},
    -- Platform sources are selected per-OS below, not by the conventional glob.
    impl = {deps = {"Core"}, files = false},
})

target("Platform.Impl")
    if is_plat("windows") then
        add_files("Windows/**.cpp")
        add_syslinks("dbghelp")
    elseif is_plat("macosx") then
        add_files("Mac/**.cpp")
    end
