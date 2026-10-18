module Platform:Mac.impl;

namespace SoulEngine {

auto InstallCrashHandler() -> void {
    // No-op on macOS. Crash dump capture (Breakpad/crashpad-style) TBD.
}

} // namespace SoulEngine
