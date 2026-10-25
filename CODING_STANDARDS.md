# Coding Standards

The repo's documented coding standards, consumed by /code-review at review time. Language- and topic-specific standards live as separate files with implementation-time reading triggers below. AGENTS.md points at the comment spec directly because comment rework is expensive.

## Standards files

- [Editor Commands](docs/standards/editor-commands.md) — shared human/Agent registration, invocation, phases, completion, and provider lifetime; read when implementing command providers, input routing, or MCP dispatch.
- [Comments](docs/standards/comments.md) — comment placement, form, and content. **Read before writing or modifying any comment** (implementation time). The mechanical width and form rules are enforced by `Tools/check_comments.py`.
- [C++ index](docs/standards/cpp/index.md) — **read for every C++ change**; it contains high-frequency rules and explicit triggers for error paths, fallible construction, modules, and Tracy profiling.
- [Slang](docs/standards/slang.md) — runtime shader rules, resource binding, modules, naming.
- [Vulkan](docs/standards/vulkan.md) — backend API usage rules (StructureChain, timeline semaphores, object naming, Vulkan-Hpp).
- [xmake](docs/standards/xmake.md) — module declaration (`soul_module`), visibility rule, test registration, final-binary wiring.

## Project-wide standards

### Test fixtures are self-contained

ShaderCompiler tests must only compile self-contained fixtures under `Engine/Source/Runtime/ShaderCompiler/Tests/` — never runtime engine or application shaders (`Engine/Shaders/`, `Applications/`). Runtime shaders move, split into modules, and change with renderer work, which breaks tests for unrelated reasons. Each fixture models one compiler capability at a time and must not mirror the resource surface or entry-point set of a runtime program. Fixture paths reach the tests through the environment variables xmake sets for the test target; tests must not construct paths into the engine or application trees.
