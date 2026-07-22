target("Material")
    set_kind("moduleonly")

    add_packages("hlslpp", {public = true})
    add_deps("Core", {public = true})

    add_files("*.cppm")
