-- Test helper function to create test targets for each module
-- C++ module BMI/object ownership is target-scoped in xmake. Keep every test file
-- as a real binary target instead of registering many files on one aggregate test
-- target; otherwise stale or mixed module artifacts can be reused across tests.
function add_package_rpath(package_names)
    on_load(function(target)
        for _, package_name in ipairs(package_names) do
            local package = target:pkg(package_name)
            if package then
                for _, linkdir in ipairs(package:get("linkdirs")) do
                    target:add("rpathdirs", linkdir, {force = true})
                end
            end
        end
    end)
end

function test_module(module_name, opt)
    opt = opt or {}

    local deps = opt.deps or opt.additional_deps or {}
    table.insert(deps, module_name)
    local packages = opt.packages or opt.additional_packages or {}
    table.insert(packages, "gtest")
    local tests_dir = path.absolute(path.join(os.scriptdir(), "Tests"))
    local exclude_patterns = opt.exclude_tests or {}
    -- Test base-name patterns dropped when the active toolchain is a clang
    -- frontend (any driver style).  Evaluated per target in on_config: the
    -- configured toolchain name is not visible at script scope in xmake v3
    -- (get_config("toolchain") returns nil), so the check must run where
    -- target:toolchains() is resolved.
    local exclude_on_clang = opt.exclude_on_clang or {}
    for _, testfile in ipairs(os.files("Tests/**.cpp")) do
        local basename = path.basename(testfile)
        local excluded = false
        for _, pattern in ipairs(exclude_patterns) do
            if basename:match(pattern) then
                excluded = true
                break
            end
        end
        if not excluded then
            local test_target_name = "TestsFor" .. module_name .. "_" .. basename
            target(test_target_name)
                set_kind("binary")
                set_default(false)
                add_deps(table.unpack(deps))
                add_packages(table.unpack(packages))
                -- gtest[main] supplies gmock_main. Xmake places static libraries
                -- before test objects on MSVC, so force-link that archive instead
                -- of adding a project-local main.  Driver style decides the flag
                -- form: link.exe / clang-cl accept /WHOLEARCHIVE directly; the
                -- GNU-style clang++ driver (toolchain "clang"/"gcc" on Windows)
                -- needs -Wl, forwarding.  Both decisions need the resolved
                -- toolchain, so they run in on_config (see exclude_on_clang).
                on_config(function(target)
                    local clang_frontend = false
                    local gnu_driver     = false
                    for _, toolchain_inst in pairs(target:toolchains()) do
                        local name = toolchain_inst:name()
                        if name == "clang" or name == "llvm" then
                            clang_frontend = true
                        end
                        if name == "clang" or name == "gcc" then
                            gnu_driver = true
                        end
                    end

                    -- Clang-frontend exclusions (e.g. the clang 23.1.1
                    -- optimizer SEGV workaround in Scene).  Disabled targets
                    -- must not register tests either, or `xmake test` runs a
                    -- binary that was never built.
                    local disabled = false
                    for _, pattern in ipairs(exclude_on_clang) do
                        if basename:match(pattern) then
                            if clang_frontend then
                                target:set("enabled", false)
                                disabled = true
                            end
                            break
                        end
                    end

                    if not disabled then
                        target:add("tests", "default", {
                            group = module_name,
                            runenvs = {
                                SOUL_ENGINE_TEST_SOURCE_DIR = tests_dir
                            }
                        })
                    end

                    -- Windows-only: Apple/GNU ld pull main() from the archive
                    -- normally; /WHOLEARCHIVE is a link.exe flag and the .lib
                    -- name is MSVC-specific.
                    if target:is_plat("windows") then
                        if gnu_driver then
                            target:add("ldflags", "-Wl,/WHOLEARCHIVE:gmock_main.lib", {force = true})
                        else
                            target:add("ldflags", "/WHOLEARCHIVE:gmock_main.lib", {force = true})
                        end
                    end
                end)
                if opt.rpath_packages then
                    add_package_rpath(opt.rpath_packages)
                end
                add_files(testfile)
        end
    end
end

includes("Core")
includes("Platform")
includes("WindowSystem")
includes("Shader")
includes("RHI")
includes("ShaderCompiler")
includes("Material")
includes("Scene")
includes("TaskGraph")
includes("Application")
-- RenderGraph is a Renderer dependency; declare it before Renderer so target
-- and C++ module dependency order matches (RenderGraph only needs Core + RHI).
includes("RenderGraph")
includes("Renderer")
includes("Editor")
includes("Launch")

target("SoulEngine")
    set_default(true)
    set_kind("binary")

    add_deps("Core", "Launch")
    add_files("main.cpp")

    -- ShaderCompiler is a moduleonly target; its shared-library packages
    -- (slang) are not linked through an archive, so xmake does not
    -- automatically propagate their lib dirs into the consumer RPATH.
    -- Add slang's libdir explicitly so dyld can resolve
    -- @rpath/libslang-compiler.*.dylib at launch without DYLD_LIBRARY_PATH
    -- (needed by IDE debuggers like VSCode/Zed which don't inherit env).
    add_packages("slang")
    add_package_rpath({"slang"})
