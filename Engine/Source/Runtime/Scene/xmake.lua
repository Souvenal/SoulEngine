target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp", "entt", "assimp", {public = true})
    add_deps("Core", "Material", "Resource", {public = true})

    add_files("*.cppm")



test_module("Scene", {additional_packages = {"entt"}})
