// Required while EngineLoop's ownership chain reaches Scene's exposed entt::registry.
// Constructing EngineLoop can instantiate that lifetime chain in this translation unit.
#include <entt/entt.hpp>

import Core;
import Launch;

auto main(int argc, char* argv[]) -> int {
    using namespace SoulEngine;

    std::span CmdLineArgs{argv, static_cast<std::size_t>(argc)};

    EngineLoop GEngineLoop;

    if (auto R = GEngineLoop.PreInit(CmdLineArgs); !R) {
        LogError("{}", R.error().ToString());
        LogInfo("Engine exited after `PreInit`");
        return 1;
    }

    if (auto R = GEngineLoop.Init(); !R) {
        LogError("{}", R.error().ToString());
        LogInfo("Engine exited after `Init`");
        return 1;
    }

    GEngineLoop.Run();
    LogInfo("Engine exited after `Run`");
    return 0;
}
