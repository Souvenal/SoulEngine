-- EditorTypes is a second, interface-only module living in this directory; the
-- explicit files list keeps Editor's own interface units out of it.
soul_module("EditorTypes", {
    deps  = {"Core", "RHI", "Scene"},
    files = "EditorTypes.cppm",
})

soul_module("Editor", {
    deps     = {"Application", "Core", "EditorTypes", "Renderer", "RHI", "Scene", "TaskGraph", "WindowSystem"},
    -- public: consumers (Launch -> SoulEngine.exe) must link the imgui archive.
    packages = {"magic_enum", "entt", "hlslpp", "imgui", "imgui-club"},
    excludes = {"EditorTypes.cppm"},
    impl = {
        deps     = {"Application", "Core", "Renderer", "RHI", "Scene", "TaskGraph", "WindowSystem"},
        packages = {"magic_enum", "entt", "hlslpp", "imgui", "imgui-club"},
    },
    tests = {additional_packages = {"imgui", "imgui-club"}},
})
