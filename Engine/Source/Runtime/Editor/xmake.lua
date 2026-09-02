target("EditorTypes")
    set_kind("moduleonly")
    add_deps("Core", "RHI", "Scene")
    add_files("EditorTypes.cppm")

target("Editor")
    set_kind("moduleonly")

    add_deps("Application", "Core", "EditorTypes", "Renderer", "RHI", "Resource", "Scene", "TaskGraph", "WindowSystem")
    add_packages("magic_enum", {public = true})
    add_packages("entt")
    add_packages("hlslpp", {public = true})
    -- public: consumers (Launch -> SoulEngine.exe) must link the imgui archive.
    add_packages("imgui", "imgui-club", {public = true})

    add_files("Editor.cppm", "EditorWorld.cppm", "MainMenu.cppm", "UIManager.cppm", "UIPanels.cppm")

test_module("Editor", {
    additional_deps = {"Editor"},
    additional_packages = {"imgui", "imgui-club"}
})
