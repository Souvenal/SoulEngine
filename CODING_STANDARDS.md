# Coding Standards

Repo-wide judgement-call standards, read at review time (by /code-review), not during implementation. Path-scoped mechanical rules live in [.agents/rules/](.agents/rules/) instead; anything a deterministic check can enforce should become one.

## Test fixtures are self-contained

ShaderCompiler tests must only compile self-contained fixtures under `Engine/Source/Runtime/ShaderCompiler/Tests/` — never runtime engine or application shaders (`Engine/Shaders/`, `Applications/`). Runtime shaders move, split into modules, and change with renderer work, which breaks tests for unrelated reasons. Each fixture models one compiler capability at a time and must not mirror the resource surface or entry-point set of a runtime program. Fixture paths reach the tests through the environment variables xmake sets for the test target; tests must not construct paths into the engine or application trees.
