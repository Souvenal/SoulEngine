---
paths:
  - "Engine/Shaders/**/*.slang"
  - "Applications/*/Shaders/**/*.slang"
---

# Slang Shader Coding Specifications

## Scope

Runtime shader rules for SoulEngine. Prefer Slang cross-platform features. Backend-specific syntax only in compiler/backend fixtures.

## Core Rules

- No `cbuffer` / `tbuffer`. Use `struct` + `ParameterBlock<T>`.
- No runtime `register(...)`, `[[vk::...]]`, `[[spirv::...]]`, `[[binding]]`, or backend layout annotation.
- Use `: SV_Position` / `: SV_TARGET` for system-value outputs.
- Do not use `[[vk::location(N)]]` in runtime vertex input. CPU explicit vertex layout is authority; shader reflection only warning-checks location/format.
- Runtime entry points use `[shader("vertex")]`, `[shader("fragment")]`, `[shader("compute")]`, etc. `ShaderCache` compiles whole module and caches reflected entry points.
- No file-scope resources (`Texture2D`, `RWStructuredBuffer<T>`, `SamplerState`, etc.) in runtime shaders. Put resources in `ParameterBlock<T>`.
- Short one-off test shaders may violate runtime style only when testing compiler/reflection/error behavior.

## Matrix Layout

- Runtime transform matrices use row-major convention.
- Shader transform order: `mul(vector, matrix)`.
- Do not CPU-transpose normal constant-buffer matrices.
- Do not add `column_major` / `row_major` in runtime shaders to patch transform bugs.
- Decision/rationale: see `Docs/ADR/04-row-major-host-to-shader-matrix-convention.md`.

## Resource Binding

- Runtime descriptor sets use `ParameterBlock<T>`.
- Current shared runtime layout:
  - Set 0: per-frame data from `Engine/Shaders/Common.slang`.
  - Set 1: bindless sampled textures.
- Unbounded arrays (`T[]`) must be last/only field in a `ParameterBlock`; only one variable-count binding per set.
- Immutable samplers belong in sampler `ParameterBlock` when added; backend bakes immutable sampler layout.
- `[[vk::binding]]`, `[[binding]]`, `register(...)` allowed only in `Engine/Source/Runtime/ShaderCompiler/Tests/Slang/` or narrow backend/compiler fixtures.

## Modules

- Shared engine modules live in `Engine/Shaders/`.
- Application shaders live in `Applications/<App>/Shaders/`.
- Imported shared shader files declare `module <Name>;` as first non-comment line.
- Application entry shaders may omit `module` and `import` shared modules.
- Same-module extension files use `implementing <Name>;`.
- Cross-module dependency uses `import <Name>;`.
- Do not use `__include` for unrelated logic.

## Cross-Platform Rules

- Target-neutral Slang first: `ParameterBlock<T>`, modules, interfaces, portable resource types.
- Use `__target_switch` only when behavior truly differs by target.
- Avoid `#ifdef` target branching.
- `__target_intrinsic(...)` only behind small target-neutral helper/fallback, or in backend/compiler tests.
- Never use backend annotation to express ordinary runtime resource layout.

## API / Type Rules

- `public` for types/functions/globals used across module boundaries.
- Everything else private/internal by default.
- Mark `override` on interface method overrides.
- Use `static_assert` for compile-time invariants.
- Comments: `//` or `///`; no `/* */`.
- `[[unroll]]` / `[ForceInline]` only after profiling.

## Generics

- Prefer `interface` over preprocessor specialization.
- Generic value parameters use `let N : int`, not bare `int N`.
- Prefer `where` clauses for multiple constraints.
- Use `This` associated type pattern for self-referencing trait constraints.
- Differentiable code uses `[Differentiable]`; types used there implement `IDifferentiable`.

## Naming

| Category | Convention | Example |
|----------|------------|---------|
| Entry points | camelCase + stage abbrev | `vertMain`, `fragMain`, `computeMain` |
| Types | PascalCase | `FrameData`, `MaterialParams` |
| Functions | camelCase | `sampleShadowMap` |
| Variables | camelCase | `worldPos` |
| File-scope resources | `g_` prefix | `g_frame` |
| Constants | `k` + PascalCase | `kMaxLights` |
| Generic types | `T` or PascalCase with `T` prefix | `T`, `TLight` |

## Fixtures

`Engine/Source/Runtime/ShaderCompiler/Tests/Slang/` may intentionally use backend annotations, file-scope resources, invalid syntax, or legacy forms to test compiler/reflection/error paths. Do not copy fixture style into runtime shaders.

