module Platform:Mac;

namespace Platform {

auto InstallCrashHandler() -> void {
    // No-op on macOS. Crash dump capture (Breakpad/crashpad-style) TBD.
}

} // namespace Platform
