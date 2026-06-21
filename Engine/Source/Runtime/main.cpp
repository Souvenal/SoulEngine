import Core;
import Launch;

auto main(int argc, char* argv[]) -> int {
    using namespace SoulEngine;

    std::span CmdLineArgs{argv, static_cast<std::size_t>(argc)};

    Launch::EngineLoop GEngineLoop;

    if (auto R = GEngineLoop.PreInit(CmdLineArgs); !R) {
        LogError("{}", R.error().ToString());
        return 1;
    }

    if (auto R = GEngineLoop.Init(); !R) {
        LogError("{}", R.error().ToString());
        return 1;
    }

    GEngineLoop.Run();
    LogInfo("Engine shutdown complete");
    return 0;
}
