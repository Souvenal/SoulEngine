target("TaskGraph")
    set_kind("moduleonly")
    set_policy("build.c++.modules", true)

    add_deps("Core")
    add_packages("magic_enum", "tracy", {public = true})
    add_files("*.cppm", "*.mpp")

test_module("TaskGraph", {
    deps = {"TaskGraph"}
})
