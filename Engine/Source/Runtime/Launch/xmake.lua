target("Launch")
    set_kind("moduleonly")

    add_packages("tracy")
    add_deps("Core", "Platform", "Window", "Application", "RHI")
    
    add_files("**.cppm")
