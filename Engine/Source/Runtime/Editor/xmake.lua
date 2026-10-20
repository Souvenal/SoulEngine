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
    add_files("*.cpp")

test_module("Editor", {
    additional_deps = {"Editor"},
    additional_packages = {"imgui", "imgui-club"}
})
