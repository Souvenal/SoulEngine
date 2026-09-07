# AGENTS.md

Guidance for AI agents on this repo.

## Build & Run

- Configure: `xmake f -m debug` or `xmake f -m release`
- Build: `xmake`
- Run: `xmake run`
- Run tests: `xmake test`
- Binary: `Engine/Binaries/SoulEngine`
- Compiler: Clang 20+
- C++23 modules enabled (`import std;` throughout)
- **Agent command output:** `rtk` is an optional command-output filter. When
  `rtk` is available on `PATH`, build and test commands MUST use `rtk err`
  (for example, `rtk err xmake -y` and `rtk err xmake test`). When it
  is unavailable, run the equivalent xmake command directly.

## Project Structure

- `Engine/Source/Runtime/Core/` — Foundation module. Utility types, logging (spdlog), toml++ config. Every module depends on this.
- `Engine/Source/Runtime/WindowSystem/` — Window-system (WIS) abstraction. GLFW implementation behind `IWindowSystem`; dispatch on `WindowSystemType` for native handles.
- `Engine/Source/Runtime/RHI/` — Vulkan rendering hardware interface.
- `Engine/Source/Runtime/Shader/` — Shader type system (descriptor bindings, resource types).
- `Engine/Source/Runtime/Material/` — Renderer-neutral material data model.
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

- **Build System:** xmake `set_languages()` needs string quoting (e.g., `set_languages('c++23')`, NOT `set_languages(c++23)`). Clean generated build artifacts (`xmake clean --all`) when C++ module compilation gives inexplicable errors.
- **Dear ImGui:** built from a local source checkout (core and the `imgui_impl_glfw` backend must share one version), not xrepo. Path configurable via `xmake f --imgui_dir=<path>` (default `C:/Users/22067/Projects/imgui`).
- **Package Dependencies:** When using local xmake package repos, prefer inline `package()` definitions in `xmake.lua` over `add_repositories()` — xmake may silently ignore local repos for official ones.

## Subagent Delegation (MANDATORY)

Before doing ANY non-trivial work, evaluate whether a subagent is more appropriate.
Delegation is the DEFAULT; doing it inline is the exception.

### Must delegate (spawn subagent)

| Task | Subagent | Notes |
|------|----------|-------|
| Locate code / find definitions / list callers | `cavecrew-investigator` | Returns path:line + symbol |
| Surgical edit ≤2 files, site already known | `cavecrew-builder` | Returns path:range + change summary |
| Review a diff or file for bugs | `cavecrew-reviewer` | Returns findings with severity |
| Research task that doesn't need full context | `subagent` (generic) | Self-contained prompt |

### Prefer delegation

| Task | Approach |
|------|----------|
| Multi-step with independent subtasks | Spawn multiple subagents in parallel |
| Subagent output is self-contained | Use `subagent` (background) |
| Subtask builds on conversation context | Use `subagent_fork` (background) |

### Do inline (exception)

| Task | Why |
|------|-----|
| Single-line answer you already know | Trivial |
| Real-time back-and-forth with user | Needs conversation context |
| Full conversation context is essential | Subagent can't see it |

### Chaining pattern (locate → fix → verify)

1. Spawn `cavecrew-investigator` to find sites.
2. Pick 1-2 sites, spawn `cavecrew-builder` for edits.
3. Spawn `cavecrew-reviewer` to audit the diff.

Do NOT do all three steps yourself when delegation is cleaner.

### Parallel patterns

- When 2+ subtasks are independent, spawn them ALL in one message.
- Use `subagent` for isolated tasks, `subagent_fork` when context matters.
- Default `run_in_background: true` unless you need the result immediately.

### Output contracts

- **Investigator:** file paths with line numbers and symbols.
- **Builder:** path:line-range with change description.
- **Reviewer:** path:line with severity emoji + fix suggestion.
- Keep output terse. If a human will read it directly, paraphrase.

## Vulkan / RHI Conventions

- **Vulkan Synchronization:** WAW/WAR/RAW hazards require per-image state tracking. Prefer per-frame image state initialization over blanket AllCommands barriers. Timeline semaphores replace fences for GPU-GPU sync.
