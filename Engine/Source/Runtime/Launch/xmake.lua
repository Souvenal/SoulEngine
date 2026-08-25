target("Launch")
    set_kind("moduleonly")

    add_packages("magic_enum", {public = true})
    add_packages("tracy", {public = true})
    add_packages("imgui-club")
    add_deps("Core", "Platform", "WindowSystem", "Application", "Editor", "RHI", "Scene", "Renderer", "TaskGraph", "Resource")
    
    add_files("**.cppm")
