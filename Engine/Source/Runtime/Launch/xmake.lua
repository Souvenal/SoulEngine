target("Launch")
    set_kind("moduleonly")

    add_packages("tracy")
    add_deps("Core", "Platform", "Window", "Application", "RHI", "Scene", "Renderer", "TaskGraph")
    
    add_files("**.cppm")
