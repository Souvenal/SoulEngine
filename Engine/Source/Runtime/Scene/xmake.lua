target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp", "entt", "assimp", "magic_enum", {public = true})
    add_deps("Core", "Material", "RHI", {public = true})

    add_files("*.cppm")



test_module("Scene", {additional_packages = {"entt"}})
