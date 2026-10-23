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
includes("RenderGraph")
includes("Renderer")
includes("Editor")
includes("Launch")

soul_finalize_modules()
soul_finalize_tests()

target("SoulEngine")
    set_default(true)
    set_kind("binary")

    add_deps("Core", "Launch")
    -- Implementation targets come from the soul_module registry: every module
    -- with an impl half is linked here without a hand-maintained list.
    add_deps(table.unpack(soul_impl_targets()))
    add_files("main.cpp")

    -- ShaderCompiler is a moduleonly target; its shared-library packages
    -- (slang) are not linked through an archive, so xmake does not
    -- automatically propagate their lib dirs into the consumer RPATH.
    -- Add slang's libdir explicitly so dyld can resolve
    -- @rpath/libslang-compiler.*.dylib at launch without DYLD_LIBRARY_PATH
    -- (needed by IDE debuggers like VSCode/Zed which don't inherit env).
    add_packages("slang")
    soul_add_package_rpath({"slang"})
