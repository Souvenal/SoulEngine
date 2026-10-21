target("Launch")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_packages("magic_enum", {public = true})
    add_packages("tracy", {public = true})
    add_packages("imgui-club")
    add_deps("Core", "Platform", "WindowSystem", "Application", "Editor", "RHI", "Scene", "Renderer", "TaskGraph", "RenderGraph")

    add_files("Launch.cppm", {public = true})

target("Launch.Impl")
    set_kind("static")
    set_policy("build.c++.modules", true)
    add_deps("Launch", "Core.Impl", "Platform.Impl", "WindowSystem.Impl", "Application.Impl", "Editor.Impl", "RHI.Impl", "Scene.Impl", "Renderer.Impl", "TaskGraph.Impl", "RenderGraph.Impl")
    add_packages("magic_enum", "tracy", "imgui-club")
    add_files("Launch.cpp")
