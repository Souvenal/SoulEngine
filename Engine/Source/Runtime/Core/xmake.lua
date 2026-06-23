target("Core")
    set_kind("moduleonly")

    add_packages("spdlog", "toml++")

    add_files("**.cppm")

test_module("Core", {
    deps = {"Core"}
})
