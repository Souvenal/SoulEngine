# Coding Standards

The repo's documented coding standards, consumed by /code-review at review time. Language- and topic-specific standards live as separate files; the one implementation-time exception is the comment spec, which AGENTS.md points at directly because comment rework is expensive.

## Standards files

- [Comments](docs/standards/comments.md) — comment placement, form, and content. **Read before writing or modifying any comment** (implementation time). The mechanical width and form rules are enforced by `Tools/check_comments.py`.
- [C++](docs/standards/cpp.md) — error handling, ownership, style, module, and Tracy profiling rules.
- [Slang](docs/standards/slang.md) — runtime shader rules, resource binding, modules, naming.
- [Vulkan](docs/standards/vulkan.md) — backend API usage rules (StructureChain, timeline semaphores, object naming, Vulkan-Hpp).

## Project-wide standards

### Test fixtures are self-contained

ShaderCompiler tests must only compile self-contained fixtures under `Engine/Source/Runtime/ShaderCompiler/Tests/` — never runtime engine or application shaders (`Engine/Shaders/`, `Applications/`). Runtime shaders move, split into modules, and change with renderer work, which breaks tests for unrelated reasons. Each fixture models one compiler capability at a time and must not mirror the resource surface or entry-point set of a runtime program. Fixture paths reach the tests through the environment variables xmake sets for the test target; tests must not construct paths into the engine or application trees.
