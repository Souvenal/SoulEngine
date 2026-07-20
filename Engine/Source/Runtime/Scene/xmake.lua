target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp", "yaml-cpp", "entt", {public = true})
    add_deps("Core", "Resource", {public = true})

    add_files("*.cppm")



test_module("Scene", {additional_packages = {"entt"}})
