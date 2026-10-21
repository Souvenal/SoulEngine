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

-- Registry of every module declared through soul_module(). test targets and the
-- SoulEngine binary resolve implementation targets ("X.Impl") from this table
-- instead of a hand-maintained list. See docs/standards/xmake.md.
SoulModules = {}

-- Declares one SoulEngine C++ module:
--   target("<name>")       moduleonly interface unit; deps/packages are public
--   target("<name>.Impl")  static implementation unit (only when opt.impl is set);
--                          everything on it is private
-- opt.impl.deps lists module names; each maps to "<dep>.Impl" when that module
-- has an implementation target, otherwise the name passes through verbatim
-- (plain targets like VMA/STBImage). The interface target is prepended
-- automatically. See docs/standards/xmake.md for the full convention.
function soul_module(name, opt)
    opt = opt or {}
    local has_impl = opt.impl ~= nil and opt.impl ~= false
    SoulModules[name] = {has_impl = has_impl}

    target(name)
        set_kind("moduleonly")
        set_policy("build.c++.modules", true)
        -- The options table must ride inside the unpacked list: a multi-value
        -- call anywhere but the last argument position truncates to one value.
        if opt.deps then
            local deps = table.clone(opt.deps)
            table.insert(deps, {public = true})
            add_deps(table.unpack(deps))
        end
        if opt.packages then
            local packages = table.clone(opt.packages)
            table.insert(packages, {public = true})
            add_packages(table.unpack(packages))
        end
        if opt.defines then
            add_defines(table.unpack(opt.defines))
        end
        local interface_files = opt.files or "**.cppm"
        if opt.excludes then
            interface_files = interface_files .. "|" .. table.concat(opt.excludes, "|")
        end
        add_files(interface_files, {public = true})

    if has_impl then
        -- Deps resolve in finalize_soul_modules(): module names map to
        -- "<dep>.Impl" through the registry, which is only complete after all
        -- includes() — includes order is not topological (TaskGraph follows
        -- RHIVulkan although RHIVulkan.Impl links TaskGraph.Impl).
        SoulModules[name].impl_deps = opt.impl.deps
        local impl = opt.impl
        target(name .. ".Impl")
            set_kind("static")
            set_policy("build.c++.modules", true)
            if impl.packages then
                add_packages(table.unpack(impl.packages))
            end
            if impl.defines then
                add_defines(table.unpack(impl.defines))
            end
            if impl.files ~= false then
                local impl_files = impl.files or "**.cpp|Tests/**"
                if impl.excludes then
                    impl_files = impl_files .. "|" .. table.concat(impl.excludes, "|")
                end
                add_files(impl_files)
            end
    end

    if opt.tests then
        test_module(name, opt.tests)
    end
end

-- Adds the deferred implementation-target deps recorded by soul_module().
-- Re-entering a target appends to it; this runs after all includes() with the
-- complete registry. Module names map to "<dep>.Impl" when that module has an
-- implementation target; unregistered names (plain targets like VMA/STBImage)
-- pass through verbatim.
function finalize_soul_modules()
    for name, info in pairs(SoulModules) do
        if info.has_impl then
            local impl_deps = {name}
            for _, dep in ipairs(info.impl_deps or {}) do
                local dep_info = SoulModules[dep]
                table.insert(impl_deps, (dep_info and dep_info.has_impl) and dep .. ".Impl" or dep)
            end
            target(name .. ".Impl")
                add_deps(table.unpack(impl_deps))
            target_end()
        end
    end
end

-- Queued test registrations. Target creation is deferred until every module has
-- registered: a module's tests may link modules that are included later (RHI
-- tests link RHIVulkan.Impl), so the SoulModules lookup must run after all
-- includes() below.
SoulPendingTests = {}

function test_module(module_name, opt)
    table.insert(SoulPendingTests, {
        module_name = module_name,
        opt = opt or {},
        -- os.scriptdir() is only the module directory while the module's own
        -- xmake.lua is executing, so capture it at registration time.
        tests_dir = path.absolute(path.join(os.scriptdir(), "Tests")),
    })
end

function run_pending_tests()
    for _, pending in ipairs(SoulPendingTests) do
        local module_name = pending.module_name
        local opt = pending.opt
        local tests_dir = pending.tests_dir

        local deps = opt.deps or opt.additional_deps or {}
        table.insert(deps, module_name)
        local with_impl = {}
        for _, dep in ipairs(deps) do
            table.insert(with_impl, dep)
            local info = SoulModules[dep]
            if info and info.has_impl then
                table.insert(with_impl, dep .. ".Impl")
            end
        end
        deps = with_impl
        local packages = opt.packages or opt.additional_packages or {}
        table.insert(packages, "gtest")
        local exclude_patterns = opt.exclude_tests or {}
        -- Test base-name patterns dropped when the active toolchain is a clang
        -- frontend (any driver style).  Evaluated per target in on_config: the
        -- configured toolchain name is not visible at script scope in xmake v3
        -- (get_config("toolchain") returns nil), so the check must run where
        -- target:toolchains() is resolved.
        local exclude_on_clang = opt.exclude_on_clang or {}
        for _, testfile in ipairs(os.files(path.join(tests_dir, "**.cpp"))) do
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
end

includes("Core")
includes("Platform")
includes("WindowSystem")
includes("Shader")
includes("RHI")
includes("RHIVulkan")
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

finalize_soul_modules()
run_pending_tests()

target("SoulEngine")
    set_default(true)
    set_kind("binary")

    add_deps("Core", "Launch")
    -- Implementation targets come from the soul_module registry: every module
    -- with an impl half is linked here without a hand-maintained list.
    local impl_targets = {}
    for module_name, info in pairs(SoulModules) do
        if info.has_impl then
            table.insert(impl_targets, module_name .. ".Impl")
        end
    end
    table.sort(impl_targets)
    add_deps(table.unpack(impl_targets))
    add_files("main.cpp")

    -- ShaderCompiler is a moduleonly target; its shared-library packages
    -- (slang) are not linked through an archive, so xmake does not
    -- automatically propagate their lib dirs into the consumer RPATH.
    -- Add slang's libdir explicitly so dyld can resolve
    -- @rpath/libslang-compiler.*.dylib at launch without DYLD_LIBRARY_PATH
    -- (needed by IDE debuggers like VSCode/Zed which don't inherit env).
    add_packages("slang")
    add_package_rpath({"slang"})
