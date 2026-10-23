-- SoulEngine build helpers, loaded by the root xmake.lua via includes() before
-- any module file runs. Everything here executes in xmake's *description scope*,
-- where import() is not available — includes() is the only code-sharing channel,
-- so every global this file exports carries the soul_ prefix to stay clear of
-- xmake builtins. See docs/standards/xmake.md.

-- Registry of every module declared through soul_module(). test targets and the
-- SoulEngine binary resolve implementation targets ("X.Impl") from this table
-- instead of a hand-maintained list. See docs/standards/xmake.md.
local soul_modules = {}

-- Queued test registrations. Target creation is deferred until every module has
-- registered: a module's tests may link modules that are included later (RHI
-- tests link RHIVulkan.Impl), so the soul_modules lookup must run after all
-- includes() below.
local soul_pending_tests = {}

-- Test helper function to create test targets for each module
-- C++ module BMI/object ownership is target-scoped in xmake. Keep every test file
-- as a real binary target instead of registering many files on one aggregate test
-- target; otherwise stale or mixed module artifacts can be reused across tests.
function soul_add_package_rpath(package_names)
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

-- Queues a module's tests for soul_finalize_tests(). Local to this file:
-- modules declare tests through the soul_module() tests sub-table only.
local function queue_test_module(module_name, opt)
    table.insert(soul_pending_tests, {
        module_name = module_name,
        opt = opt or {},
        -- os.scriptdir() is only the module directory while the module's own
        -- xmake.lua is executing, so capture it at registration time.
        tests_dir = path.absolute(path.join(os.scriptdir(), "Tests")),
    })
end

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
    soul_modules[name] = {has_impl = has_impl}

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
        -- Deps resolve in soul_finalize_modules(): module names map to
        -- "<dep>.Impl" through the registry, which is only complete after all
        -- includes() — includes order is not topological (TaskGraph follows
        -- RHIVulkan although RHIVulkan.Impl links TaskGraph.Impl).
        soul_modules[name].impl_deps = opt.impl.deps
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
        queue_test_module(name, opt.tests)
    end
end

-- Adds the deferred implementation-target deps recorded by soul_module().
-- Re-entering a target appends to it; this runs after all includes() with the
-- complete registry. Module names map to "<dep>.Impl" when that module has an
-- implementation target; unregistered names (plain targets like VMA/STBImage)
-- pass through verbatim.
function soul_finalize_modules()
    for name, info in pairs(soul_modules) do
        if info.has_impl then
            local impl_deps = {name}
            for _, dep in ipairs(info.impl_deps or {}) do
                local dep_info = soul_modules[dep]
                table.insert(impl_deps, (dep_info and dep_info.has_impl) and dep .. ".Impl" or dep)
            end
            target(name .. ".Impl")
                add_deps(table.unpack(impl_deps))
            target_end()
        end
    end
end

-- Sorted names of every registered implementation target, for final binaries
-- that link all modules without a hand-maintained list.
function soul_impl_targets()
    local impl_targets = {}
    for module_name, info in pairs(soul_modules) do
        if info.has_impl then
            table.insert(impl_targets, module_name .. ".Impl")
        end
    end
    table.sort(impl_targets)
    return impl_targets
end

function soul_finalize_tests()
    for _, pending in ipairs(soul_pending_tests) do
        local module_name = pending.module_name
        local opt = pending.opt
        local tests_dir = pending.tests_dir

        local deps = opt.deps or opt.additional_deps or {}
        table.insert(deps, module_name)
        local with_impl = {}
        for _, dep in ipairs(deps) do
            table.insert(with_impl, dep)
            local info = soul_modules[dep]
            if info and info.has_impl then
                table.insert(with_impl, dep .. ".Impl")
            end
        end
        deps = with_impl
        local packages = opt.packages or opt.additional_packages or {}
        table.insert(packages, "gtest")
        local exclude_patterns = opt.exclude_tests or {}
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
                    -- toolchain, and the configured toolchain name is not visible at
                    -- script scope in xmake v3, so this runs in on_config.
                    on_config(function(target)
                        local gnu_driver = false
                        for _, toolchain_inst in pairs(target:toolchains()) do
                            local name = toolchain_inst:name()
                            if name == "clang" or name == "gcc" then
                                gnu_driver = true
                            end
                        end

                        target:add("tests", "default", {
                            group = module_name,
                            runenvs = {
                                SOUL_ENGINE_TEST_SOURCE_DIR = tests_dir
                            }
                        })

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
                        soul_add_package_rpath(opt.rpath_packages)
                    end
                    add_files(testfile)
            end
        end
    end
end
