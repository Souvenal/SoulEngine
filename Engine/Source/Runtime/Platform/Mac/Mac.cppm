module;

export module Platform:Mac;

namespace Platform {

export auto InstallCrashHandler() -> void {
    // No-op on macOS. Crash dump capture (Breakpad/crashpad-style) TBD.
}

} // namespace Platform
