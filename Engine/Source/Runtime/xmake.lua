-- Test helper function to create test targets for each module
-- C++ module BMI/object ownership is target-scoped in xmake. Keep every test file
-- as a real binary target instead of registering many files on one aggregate test
-- target; otherwise stale or mixed module artifacts can be reused across tests.
function test_module(module_name, opt)
    opt = opt or {}

    local deps = opt.deps or opt.additional_deps or {}
    table.insert(deps, module_name)
    local packages = opt.packages or opt.additional_packages or {}
    table.insert(packages, "gtest")
    local tests_dir = path.absolute(path.join(os.scriptdir(), "Tests"))
    for _, testfile in ipairs(os.files("Tests/**.cpp")) do
        local test_target_name = "TestsFor" .. module_name .. "_" .. path.basename(testfile)
        target(test_target_name)
            set_kind("binary")
            set_default(false)
            add_deps(table.unpack(deps))
            add_packages(table.unpack(packages))
            -- gtest[main] supplies gmock_main. Xmake places static libraries before
            -- test objects on MSVC, so force-link that archive instead of adding a project-local main.
            if is_plat("windows") then
                add_ldflags("/WHOLEARCHIVE:gmock_main.lib", {force = true})
            end
            add_files(testfile)
            add_tests("default", {
                group = module_name,
                runenvs = {
                    SOUL_ENGINE_TEST_SOURCE_DIR = tests_dir
                }
            })
    end
end

includes("Core")
includes("Platform")
includes("Window")
includes("Shader")
includes("RHI")
includes("ShaderCompiler")
includes("Resource")
includes("Material")
includes("Scene")
includes("TaskGraph")
includes("Application")
includes("Renderer")
includes("Launch")

target("SoulEngine")
    set_default(true)
    set_kind("binary")

    add_deps("Core", "Launch")
    add_files("main.cpp")
