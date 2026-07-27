target("Editor")
    set_kind("moduleonly")

    add_deps("Core", "RHI", "Resource", "TaskGraph", "WindowSystem")
    -- public: consumers (Launch -> SoulEngine.exe) must link the imgui archive.
    add_packages("imgui", "imgui-club", {public = true})

    add_files("*.cppm")

test_module("Editor", {
    additional_deps = {"Editor"},
    additional_packages = {"imgui", "imgui-club"}
})
