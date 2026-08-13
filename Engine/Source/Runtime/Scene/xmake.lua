target("Scene")
    set_kind("moduleonly")

    add_packages("hlslpp", "entt", {public = true})
    -- Scene's exported module interface does not expose libyaml types, but
    -- module-only targets require this link dependency to propagate to consumers.
    add_packages("libyaml", {public = true})
    add_deps("Core", "Material", "Resource", {public = true})

    add_files("*.cppm")



test_module("Scene", {additional_packages = {"entt"}})
