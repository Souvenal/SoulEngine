target("TaskGraph")
    set_kind("moduleonly")

    add_deps("Core")
    add_files("**.cppm")

test_module("TaskGraph", {
    deps = {"TaskGraph"}
})
