target("Launch")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_packages("magic_enum", {public = true})
    add_packages("tracy", {public = true})
    add_packages("imgui-club")
    add_deps("Core", "Platform", "WindowSystem", "Application", "Editor", "RHI", "Scene", "Renderer", "TaskGraph", "RenderGraph")

    add_files("Launch.cppm", {public = true})
    add_files("Launch.cpp")
