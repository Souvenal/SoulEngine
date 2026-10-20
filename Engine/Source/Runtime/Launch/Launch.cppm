export module Launch;

import Core;
import std;

namespace SoulEngine {

export class EngineLoop {
  public:
    EngineLoop();
    ~EngineLoop();

    /// @brief Process command-line arguments and load engine configuration.
    /// @details Currently only loads the engine configuration file; no
    /// command-line argument handling is implemented yet.
    [[nodiscard]] auto PreInit(std::span<char*> CmdLineArgs) -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage>;

    /// @brief Spawn worker threads and run the main-thread game loop.
    auto Run() -> void;

    auto Shutdown() -> void;

  private:
    struct Impl;
    UPtr<Impl> m_Impl;
};

} // namespace SoulEngine
