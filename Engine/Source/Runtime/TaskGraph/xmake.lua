target("TaskGraph")
    set_kind("moduleonly")

    add_deps("Core")
    add_packages("magic_enum", {public = true})
    add_files("**.cppm")

test_module("TaskGraph", {
    deps = {"TaskGraph"}
})
