# SoulEngine

SoulEngine is a personal lightweight game engine built from scratch with modern C++23 and xmake. It is first and foremost a **learning project** — a vehicle for studying low-level GPU APIs, hardware architecture, advanced rendering algorithms, and large-scale software engineering.

The project structure is inspired by Unreal Engine's modular design (see the `Engine/` directory), but where UE uses a directory convention (`Public/` vs `Private/`) to indicate module visibility, SoulEngine leverages **C++ modules** as a first-class language mechanism to enforce the same boundary natively.

## Design Pillars

- **Learning-driven** — every system is an opportunity to understand how and why it works, not just to ship a feature
- **Modular** — each engine subsystem is a separate xmake module with well-defined public/private boundaries via C++ modules
- **Cross-platform** — the RHI abstraction layer is designed for multiple backends; currently only Vulkan is implemented
- **Modern stack** — C++23 modules, xmake, Slang shaders

## Technology Stack

| Component | Choice | Note |
|-----------|--------|------|
| Language | C++23 | C++ modules enabled |
| Build system | xmake | Cross-platform build with built-in dependency management |
| Graphics API | Vulkan | Primary and only RHI backend for now |
| Shader language | Slang | Cross-platform shader compilation |
| Model import | Assimp | Runtime import for mesh assets such as OBJ |
| Dependency management | xmake packages | Global cache shared across worktrees; no submodule overhead in agent + worktree workflows |

## Project Structure

```
Engine/
├── Source/
│   ├── Runtime/
│   │   ├── Core/       -- Core utilities, logging, math, containers
│   │   ├── Engine/     -- Engine loop, world management, subsystems
│   │   ├── Launch/     -- Application entry point
│   │   └── RHI/        -- Render Hardware Interface abstraction
│   └── ThirdParty/     -- Third-party library wrappers
└── xmake.lua           -- Root build file
```

Each runtime module has its own `xmake.lua` for module-specific build configuration.

## Building

### Prerequisites

- xmake (latest)
- A C++23 modules-capable compiler
  - **Windows:** LLVM Clang is **required** — MSVC currently cannot compile the project (see [Known Issues](#known-issues)). The root `xmake.lua` already pins the `clang` toolchain, so a plain `xmake f` picks it up automatically.
  - **macOS/Linux:** Clang is the configured toolchain.
- **Vulkan SDK** — Download and install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home). Before building or running, source the environment setup script:

  ```bash
  source /path/to/VulkanSDK/x.x.x.x/setup-env.sh
  ```

  It is recommended to add this to your shell profile (e.g. `~/.zshrc` or `~/.bash_profile`) so `VULKAN_SDK` is always set:

  ```bash
  # Vulkan SDK
  source /path/to/VulkanSDK/x.x.x.x/setup-env.sh
  ```

  The `VULKAN_SDK` environment variable is how xmake locates the Vulkan headers and libraries. Without it, the build system will not find the Vulkan dependency.

  In addition, `setup-env.sh` configures the dynamic linker library path (`DYLD_LIBRARY_PATH` on macOS, `LD_LIBRARY_PATH` on Linux) so that Vulkan SDK shared libraries can be resolved at runtime. Skipping this step will result in missing dylib errors when launching the executable.

### Git Hooks

Install the local Git pre-commit hook:

```bash
uv run pre-commit install
```

### Build & Run

```bash
xmake f -m release # configure
xmake              # build
xmake run          # run SoulEngine
```

See [`Docs/platform/`](Docs/platform/) for platform-specific setup notes (e.g. macOS Vulkan driver configuration).

## Motivation

Why build a game engine from scratch instead of using an existing one?

I learn best by reading real code and writing real systems — not by studying abstractions on paper. This project exists to:

- Understand how GPU hardware actually works by programming against Vulkan directly
- Implement rendering algorithms (from basic to cutting-edge) with full control
- Learn how large C++ projects are organized, modularized, and maintained
- Explore C++23 modules in a real-world, non-trivial codebase
- Build a cross-platform graphics foundation (even if only Vulkan is active today)

## Status

Early, exploratory, single-developer. Nothing is stable. Everything is subject to change.

## Known Issues

### Windows Toolchain: Clang Required (MSVC Currently Broken)

Windows builds must use **LLVM Clang**. The root `xmake.lua` hardcodes `set_toolchains("clang")`, so the default configure already uses it — do not switch the Windows toolchain back to MSVC.

The blocker is MSVC compiler bugs with C++23 modules, not project code:

- **MSVC 14.51 (VS 2026)** fails with a fatal **C1116** IFC-merge error: when a translation unit imports several modules whose global module fragments pull in `<thread>`/`<stop_token>` internal reference records (Tracy's headers inside the `Vulkan:Profiling` partition), MSVC mis-merges the module interface files.
- **MSVC 14.44 / 14.38** fail even earlier, while compiling the `Core` module, so they are not a fallback path.

Revisit MSVC only after those compiler bugs are fixed upstream (e.g. a Visual Studio update that resolves the C1116 IFC merge), then validate a clean full-engine build before changing the toolchain pin.

## License

MIT
