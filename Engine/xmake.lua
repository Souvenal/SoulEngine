on_config(function()
    local example = path.join(os.scriptdir(), "Configs/SoulEngine.example.toml")
    local target  = path.join(os.scriptdir(), "Configs/SoulEngine.toml")
    if not os.isfile(target) then
        os.cp(example, target)
    end
end)

includes("Source")
