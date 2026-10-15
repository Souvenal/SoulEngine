# ADR 01 — SoulEngine Background and Technology Choices

**Status:** accepted

## Project Motivation

Building a game engine from scratch is a personal learning project that also serves as a portfolio piece. The learning goals cover graphics algorithms, low-level GPU APIs, how to organize a large C++ codebase, and exploring and improving software architecture. For real engineering work, established engines (Unreal, Unity, Godot) should always be the choice. The value of this project lies in having a platform where novel features — especially in graphics — can be freely developed and verified. The project structure is loosely inspired by Unreal Engine 5's module layout.

C++ was chosen because it is my most familiar language, and game engines remain one of C++'s strongest use cases.

## Technology Choices

### C++23 Modules

As of 2026, C++ module support is far from mature but usable, and most major C++ libraries provide module support. The traditional header file system has several inherent defects:

- ODR violations
- Declaration/implementation separation (changing a function signature requires toggling between header and implementation files)
- Redundant compilation from repeated includes — especially severe with monolithic headers like the Vulkan SDK
- No meaningful control over exported symbol visibility

Previous mitigations (PCH, Unity Builds, UE5's Public/Private directory split) are all workarounds. Only modules solve redundant compilation at the root, and the `export` semantics provide flexible visibility control.

Known cost: modules have essentially no support for exporting macros, consistent with the committee's intent to phase out macros. For example, Vulkan HPP configuration macros must be defined at module compilation time, not before `import`. This requires maintaining a custom xmake package for Vulkan headers.

### xmake

CMake is the C++ ecosystem standard and I have extensive experience with it, but its historical baggage is severe: a vast surface of opaque built-in variables, an everything-is-a-string type system that produces cryptic DSL, and a steep learning curve for both initial setup and later modification. xmake uses Lua — its scripting is dramatically simpler, more expressive, and more maintainable than CMake. xmake is a native build tool with no intermediate generator (Ninja, Makefile). It also serves as a full-featured package manager with a well-maintained official repository, and custom packages are straightforward to author. xmake's C++ modules support is fully adequate for this project, including the `moduleonly` target type.

### Vulkan

Choosing a modern low-level graphics API to learn left three options: DirectX 12, Vulkan, and Metal. Development happens across multiple devices (Windows, macOS, Linux, Android), and Vulkan is the only API that covers all of them. Current macOS development is a matter of convenience (daily-carry MacBook). Hardware raytracing will require a PC — neither MoltenVK nor Kosmic Krisp translates raytracing on macOS.

MoltenVK only implements the Vulkan Portability Subset, with incomplete functionality and its own historical baggage. Kosmic Krisp (Mesa 3D's macOS Vulkan implementation) promises full Vulkan conformance with no legacy constraints and has been included in the LunarG Vulkan SDK for macOS. Migration to Kosmic Krisp is the planned direction.

### Slang Shader Compiler

Slang was chosen over writing raw HLSL/GLSL/SPIR-V. Two reasons: a consistent preference for trying new technology, and Slang's aggressive support for neural rendering — NVIDIA and Khronos are collaborating on related standards, and neural rendering is on this project's roadmap.

### Exceptions and RTTI

The strengths and weaknesses of exceptions both stem from the same behavior: they hide what errors an interface might produce. The caller gets convenience at the cost of ignorance. C++23's `std::expected<T, E>` — borrowed from Rust — forces the caller to consider failure cases and is a strictly better design. Strong exception safety is practically impossible to guarantee in real codebases, and constructors in particular cannot be made exception-safe. The project's standard pattern is `static Create()` factory methods returning `std::expected`, which sidesteps the constructor problem entirely (see [ADR 02](02-static-create-factory.md)). Error-path performance is not a relevant factor in this decision.

RTTI is disabled per game-engine convention.

### Third-Party Dependency Philosophy

Core systems (engine architecture, RHI, render pipeline, scene organization, handle mechanisms) are built in-house for learning and ownership. Well-defined functional modules (parsing, windowing, math, testing, profiling) use established libraries.

Major third-party dependencies:
- **spdlog** — logging
- **toml++** — config parsing
- **glfw** — windowing
- **Vulkan SDK / VMA** — GPU management and rendering
- **Slang** — shader compilation
- **tracy** — profiling
- **gtest** — unit testing
