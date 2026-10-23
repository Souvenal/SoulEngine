module;

export module Platform;

import Core;
import std;

namespace SoulEngine {

/// Install the platform crash handler.
export auto InstallCrashHandler() -> void;

/// Compile-time OS detection.
export enum class PlatformOS {
    Unknown = 0,
    Windows,
    Linux,
    macOS,
};

/// Detect target OS at compile time.
export constexpr auto GetOS() -> PlatformOS {
#if defined(_WIN32)
    return PlatformOS::Windows;
#elif defined(__linux__)
    return PlatformOS::Linux;
#elif defined(__APPLE__)
    return PlatformOS::macOS;
#else
    return PlatformOS::Unknown;
#endif
}

} // namespace SoulEngine
