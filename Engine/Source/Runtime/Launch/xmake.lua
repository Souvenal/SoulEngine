target("Launch")
    set_kind("moduleonly")

    add_packages("tracy", {public = true})
    add_deps("Core", "Platform", "Window", "Application", "RHI", "Scene", "Renderer", "TaskGraph", "Resource")
    
    add_files("**.cppm")
