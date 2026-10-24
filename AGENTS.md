# AGENTS.md

Guidance for AI agents on this repo.

## Development Workflow

Never edit files in the primary worktree. Every agent-initiated change — code, documentation, comments, glossary, ADR, CI, configuration — starts with a GitHub issue and a dedicated worktree from fetched `origin/main`; this includes writes produced during design-discussion sessions. Follow [`docs/development/agent-workflow.md`](docs/development/agent-workflow.md) for issue-first scope discussion, dedicated worktrees, review handoff, maintainer approval, integration, and cleanup.

## Build & Run

- Sync package repos: `xmake repo -u` (required before configuring — the project's souvenal-repo clone lives under `.xmake/`, so any fresh checkout or deleted `.xmake` needs this; xmake does not auto-clone it when the global network mode is `private`, and the resulting "package not found in any repository" error does not mention the real cause)
- Configure: `xmake f -m debug` or `xmake f -m release`
- Build: `xmake`
- Run: `xmake run`
- Run tests: `xmake test`
- Binary: `Engine/Binaries/SoulEngine`
- Compiler: Clang 23+
- C++23 modules enabled (`import std;` throughout)
- **Agent command output:** `rtk` is an optional command-output filter. When `rtk` is available on `PATH`, build and test commands MUST use `rtk err` (for example, `rtk err xmake -y` and `rtk err xmake test`). When it is unavailable, run the equivalent xmake command directly.

## Project Structure

- `Engine/Source/Runtime/Core/` — Foundation module. Utility types, logging (spdlog), toml++ config. Every module depends on this.
- `Engine/Source/Runtime/WindowSystem/` — Window-system (WIS) abstraction. GLFW implementation behind `IWindowSystem`; dispatch on `WindowSystemType` for native handles.
- `Engine/Source/Runtime/RHI/` — Abstract GPU rendering hardware interface (resources, views, frame packets, submission contracts).
- `Engine/Source/Runtime/RHIVulkan/` — Vulkan RHI backend. Self-registers through the RHI factory; C++ module `RHIVulkan`.
- `Engine/Source/Runtime/Shader/` — Shader type system (descriptor bindings, resource types).
- `Engine/Source/Runtime/Material/` — Renderer-neutral material data model.
- `Engine/Source/Runtime/ShaderCompiler/` — Slang-based shader offline compiler + test suite (gtest).
- `Engine/Source/Runtime/Application/` — Application framework integrating RHI + shader compilation.
- `Engine/Source/Runtime/Launch/` — Engine bootstrap, main loop. Top-level runtime module.
- `Engine/Source/ThirdParty/` — xmake.lua only. Deps via xrepo (tracy, spdlog, toml++, vulkansdk, glfw, gtest).
- `Engine/Config/` — `SoulEngine.toml` runtime config, `SoulEngine.example.toml` reference.
- `GLOSSARY-MAP.md` (root) — Index of per-module `GLOSSARY.md` files and their relationships.

## Agent skills

### Issue tracker

Issues live in this repo's GitHub Issues; all operations use the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical roles, label string equals role name: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Multi-context — root `GLOSSARY-MAP.md` indexes one `GLOSSARY.md` per module; ADRs live in `docs/adr/`. See `docs/agents/domain.md`.

## Architecture & Design

- **Architecture:** Before new class/module, stop + ask clarifying questions about design intent, ownership, lifecycle. Do NOT jump to implementation.
- **Dependency Validation:** Before linking/depending on library, check: (1) runtime dlopen alternative? (2) dual-loading conflicts?

## C++ Module Conventions

- **Module Organization:** New Vulkan/RHI module partitions go in own `.cppm` file, not appended to existing modules. Partitions use `export module RHIVulkan:PartitionName;` syntax.

## Coding Specs

For code work and review, use [`CODING_STANDARDS.md`](CODING_STANDARDS.md), the index to the per-language specs consumed by /code-review. Before writing or modifying any comment, read [`docs/standards/comments.md`](docs/standards/comments.md); its mechanical rules are enforced by `Tools/check_comments.py`.

## Code Editing Rules

- **Comment Preservation:** NEVER delete/modify user-written comments (especially `// Note:`) unless asked. Preserve ALL existing comments verbatim. Exception: a change that falsifies a comment's premise owns updating it — see docs/standards/comments.md.

## Build System Notes

- **Build System:** xmake `set_languages()` needs string quoting (e.g., `set_languages('c++23')`, NOT `set_languages(c++23)`). Clean generated build artifacts (`xmake clean --all`) when C++ module compilation gives inexplicable errors.
- **Package Dependencies:** When using local xmake package repos, prefer inline `package()` definitions in `xmake.lua` over `add_repositories()` — xmake may silently ignore local repos for official ones.

## Vulkan / RHI Conventions

- **Vulkan Synchronization:** WAW/WAR/RAW hazards require per-image state tracking. Prefer per-frame image state initialization over blanket AllCommands barriers. Timeline semaphores replace fences for GPU-GPU sync.
