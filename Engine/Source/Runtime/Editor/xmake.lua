target("EditorTypes")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core", "RHI", "Scene")
    add_files("EditorTypes.cppm", {public = true})

target("Editor")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Application", "Core", "EditorTypes", "Renderer", "RHI", "Scene", "TaskGraph", "WindowSystem")
    add_packages("magic_enum", {public = true})
    add_packages("entt", {public = true})
    add_packages("hlslpp", {public = true})
    -- public: consumers (Launch -> SoulEngine.exe) must link the imgui archive.
    add_packages("imgui", "imgui-club", {public = true})

    add_files("**.cppm|EditorTypes.cppm", {public = true})

target("Editor.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("Editor", "Application.Impl", "Core.Impl", "Renderer.Impl", "RHI.Impl", "Scene.Impl", "TaskGraph.Impl", "WindowSystem.Impl")
    add_packages("magic_enum", "entt", "hlslpp", "imgui", "imgui-club")
    add_files("*.cpp")

test_module("Editor", {
    additional_deps = {"Editor", "Editor.Impl"},
    additional_packages = {"imgui", "imgui-club"}
})
