target("Editor")
    set_kind("moduleonly")

    add_deps("Application", "Core", "Renderer", "RHI", "Resource", "Scene", "TaskGraph", "WindowSystem")
    add_packages("magic_enum", {public = true})
    add_packages("entt")
    add_packages("hlslpp", {public = true})
    -- public: consumers (Launch -> SoulEngine.exe) must link the imgui archive.
    add_packages("imgui", "imgui-club", {public = true})

    add_files("*.cppm")

test_module("Editor", {
    additional_deps = {"Editor"},
    additional_packages = {"imgui", "imgui-club"}
})
