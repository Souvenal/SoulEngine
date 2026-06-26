target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp")
    add_deps("Core", "Resource", {public = true})

    add_files("Scene.cppm")
