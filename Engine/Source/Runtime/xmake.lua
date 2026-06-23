-- Test helper function to create test targets for each module
function test_module(module_name, opt)
    opt = opt or {}

    local test_target_name = "TestsFor" .. module_name
    local deps = opt.additional_deps or {}
    table.insert(deps, module_name)
    local packages = opt.additional_packages or {}
    table.insert(packages, "gtest")
    local tests_dir = path.absolute(path.join(os.scriptdir(), "Tests"))

    target(test_target_name)
        set_default(false)
        add_deps(table.unpack(deps))

        for _, testfile in ipairs(os.files("Tests/**.cpp")) do
            add_tests(path.basename(testfile), {
                group = module_name,
                files = testfile,
                packages = packages,
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
includes("ShaderCache")
includes("Scene")
includes("Resource")
includes("TaskGraph")
includes("Application")
includes("Renderer")
includes("Launch")

target("SoulEngine")
    set_default(true)
    set_kind("binary")

    add_deps("Core", "Launch")
    add_files("main.cpp")
