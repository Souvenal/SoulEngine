target("TaskGraph")
    set_kind("static")

    set_policy("build.c++.modules", true)

    add_deps("Core")
    add_packages("magic_enum", "tracy", {public = true})
    add_files("TaskGraph.cppm", {public = true})
    add_files("TaskGraph.cpp")

test_module("TaskGraph", {
    deps = {"TaskGraph"}
})
