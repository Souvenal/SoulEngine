# AGENTS.md

Guidance for AI agents on this repo.

## Build & Run

- Configure: `xmake f -m debug` or `xmake f -m release`
- Build: `xmake`
- Run: `xmake run`
- Run tests: `xmake test -v`
- Binary: `Engine/Binaries/SoulEngine`
- Compiler: Clang 20+
- C++23 modules enabled (`import std;` throughout)

## Project Structure

- `Engine/Source/Runtime/Core/` — Foundation module. Utility types, logging (spdlog), toml++ config. Every module depends on this.
- `Engine/Source/Runtime/Window/` — SDL/GLFW windowing abstraction.
- `Engine/Source/Runtime/RHI/` — Vulkan rendering hardware interface.
- `Engine/Source/Runtime/Shader/` — Shader type system (descriptor bindings, resource types).
- `Engine/Source/Runtime/ShaderCompiler/` — Slang-based shader offline compiler + test suite (gtest).
- `Engine/Source/Runtime/Application/` — Application framework integrating RHI + shader compilation.
- `Engine/Source/Runtime/Launch/` — Engine bootstrap, main loop. Top-level runtime module.
- `Engine/Source/ThirdParty/` — xmake.lua only. Deps via xrepo (tracy, spdlog, toml++, vulkansdk, glfw, gtest).
- `Engine/Config/` — `SoulEngine.toml` runtime config, `SoulEngine.example.toml` reference.
- `CONTEXT.md` (root) — Context map linking to per-module `CONTEXT.md` files.

## Agent skills

### Issue tracker

Issues as GitHub Issues via `gh` CLI. See `Docs/agents/issue-tracker.md`.

### Triage labels

GitHub defaults: `bug`, `documentation`, `enhancement`. See `Docs/agents/triage-labels.md`.

### Domain docs

Multi-context — `CONTEXT.md` at root maps to per-module `CONTEXT.md`. See `Docs/Agents/Domain.md`.
Before reading, modifying, reviewing, or generating project code, read root `CONTEXT.md`, `CONTEXT-MAP.md`, and every per-module `CONTEXT.md` that contains the target files.

## Architecture & Design

- **Architecture:** Before new class/module, stop + ask clarifying questions about design intent, ownership, lifecycle. Do NOT jump to implementation.
- **Dependency Validation:** Before linking/depending on library, check: (1) runtime dlopen alternative? (2) dual-loading conflicts?

## C++ Module Conventions

- **Module Organization:** New Vulkan/RHI module partitions go in own `.cppm` file, not appended to existing modules. Partitions use `export module Vulkan:PartitionName;` syntax.

## Code Editing Rules

- **Comment Preservation:** NEVER delete/modify user-written comments (especially `// Note:`) unless asked. Preserve ALL existing comments verbatim.

## Build System Notes

- **Build System:** xmake `set_languages()` needs string quoting (e.g., `set_languages('c++23')`, NOT `set_languages(c++23)`). Clean module cache (`xmake f -c`) when C++ module compilation gives inexplicable errors.
- **Package Dependencies:** When using local xmake package repos, prefer inline `package()` definitions in `xmake.lua` over `add_repositories()` — xmake may silently ignore local repos for official ones.

## Vulkan / RHI Conventions

- **Vulkan Synchronization:** WAW/WAR/RAW hazards require per-image state tracking. Prefer per-frame image state initialization over blanket AllCommands barriers. Timeline semaphores replace fences for GPU-GPU sync.
