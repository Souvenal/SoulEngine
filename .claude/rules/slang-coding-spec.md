---
paths:
  - "Engine/Source/**/*.slang"
---

# Slang Shader Coding Specifications

## Behavior-Changing Rules

Program semantics or compiler diagnostics rules.

### General

- **No legacy HLSL `cbuffer`/`tbuffer` syntax.** Use `struct` + `ParameterBlock<T>` or explicit uniform buffers via `[[binding(...)]]`. Rationale: `cbuffer` is HLSL legacy; Slang's parameter-block and struct-based binding give better composability and reflection.

- **No `register(...)` annotations.** Let Slang auto-assign bindings, or use `[[binding(N, set)]]` when VK/D3D12 layout must be explicit. Rationale: `register(c0)` is HLSL legacy; automatic layout simplifies cross-target porting and avoids register-space conflicts across module boundaries.

- **Use colon-syntax semantics for varying parameters and struct members.** Slang uses the standard HLSL semantic syntax (`: POSITION`, `: TEXCOORD0`, `: SV_Position`) for vertex inputs, fragment outputs, and inter-stage varyings. This applies both to entry-point parameters and to struct fields used as varying types. For Vulkan vertex attribute location numbering, combine semantics with `[[vk::location(N)]]`.

- **No legacy `[shader("...")]` on entry points** where possible. Prefer the explicit entry-point model: define a function and declare it as entry via the compiler API. When the `[shader("...")]` attribute is needed for simple single-entry shaders, keep it; for library shaders or multi-entry modules, remove it and use compiler-side entry-point selection.

- **No `Texture2D` / `RWStructuredBuffer<T>` / etc. at file scope** in new module-based shaders. Move all resource declarations into explicitly-passed struct parameters or `ParameterBlock<T>` fields. Rationale: file-scope resources are essentially implicit globals that defeat Slang's module-level reasoning and prevent separate compilation optimizations. *Exception:* very short one-off test shaders may keep file-scope resources for brevity.

### Access Control

- Use `public` for any type, function, or global that must be visible across module boundaries. Everything else stays private by default (Slang's default is `__internal`).

```hlsl
// Good — explicit access
public struct LightData { float3 direction; float intensity; };
__internal float3 halfVector(float3 L, float3 V);

// Bad — relying on default access for cross-module use
struct LightData { ... }; // not visible outside its own module
```

- Mark `override` on every interface-method implementation that overrides a default. (Slang requires it when a default body exists; be consistent even when the interface does not provide a default.)

### Error Handling in Shaders

- Use `static_assert` for compile-time invariants (thread group sizes, buffer strides, alignment, resource counts).
- Only use `//` or `///` line comments. Never use `/* */` block comments — they interfere with Slang's lexical-depth analysis and interact poorly with `__include` preprocessing.
- `[[unroll]]` and `[ForceInline]` may be applied where profiling shows a benefit; do not scatter them speculatively.

### Generics and Interfaces

- Use `interface` over preprocessor-based specialization (`#define LIGHT_TYPE_DIRECTIONAL` etc.). Interfaces enable compile-time dispatch without combinatorial macro explosion.

- Use `let N : int` for compile-time integer parameters, not bare `int N` in the generic parameter list. The `let` prefix is required by slang for value parameters:

```hlsl
// Good
void dispatch<let BlockSize : int>(uint3 dtid : SV_DispatchThreadID)
{
    shared float cache[BlockSize];
}

// Bad — legacy syntax, may work but is deprecated
void dispatch<int BlockSize>(uint3 dtid : SV_DispatchThreadID);
```

- Prefer `where` clauses over long inline constraints when a generic parameter has multiple bounds:

```hlsl
// Good
float sample<T>(T s) where T : ISampler, T : IComparable
    { ... }

// Avoid — less readable with many constraints
float sample<T : ISampler & IComparable>(T s)
    { ... }
```

- Use `This` associated type pattern for self-referencing trait constraints (common in composable shading):

```hlsl
interface ILight
{
    associatedtype Differential : IDifferentiable;
    float3 evaluate(float3 pos);
}
```

### Automatic Differentiation

- Annotate differentiable functions and types with `[Differentiable]`:

```hlsl
[Differentiable]
float3 evaluateBSDF(float3 wi, float3 wo)
{
    ...
}
```

- Types used in differentiable code must implement `IDifferentiable`. Use `__init` to define the synthesized differential type's constructor:

```hlsl
struct MyParams : IDifferentiable
{
    float3 albedo;
    float roughness;

    // Access the synthesized differential with .d
};
```

### Module System

- Every `.slang` file in `Engine/Shaders/` that belongs to a multi-file module must have `module <name>;` as its first non-comment line.
- Additional files in the same module use `implementing <name>;`.
- Dependencies between modules use `import <name>;`.

```hlsl
// scene.slang
module scene;
__include "scene-helpers";

public struct SceneData { float3 cameraPos; float4x4 viewProj; };

// scene-helpers.slang
implementing scene;

public float3 worldToView(float3 worldPos, float4x4 viewProj)
{
    return mul(viewProj, float4(worldPos, 1.0)).xyz;
}

// main.slang — consumer
import scene;

SceneData g_scene : parameterBlock(0);
```

- Do **not** use `__include` to stitch together unrelated logic. Each `__include`'d file contributes to the same module; they share a namespace. Use `import` for cross-module dependencies.

### Target Abstraction

- Use `__target_switch` when behavior must differ between targets (Vulkan vs. D3D12 vs. Metal). Avoid `#ifdef`-style preprocessor branching on target macros — `__target_switch` is cleaner and Slang's optimizer handles dead-code elimination:

```hlsl
uint waveSize()
{
    __target_switch
    {
    case spirv:
        return SubgroupSize; // SPIR-V built-in

    case hlsl:
        return WaveGetLaneCount();

    default:
        return 64;
    }
}
```

- When target-specific intrinsics are required, use Slang's target-intrinsic mechanism (`__target_intrinsic(spirv, "OpX")`) rather than string-hacking with `#define`. This keeps the Slang source valid on all targets while mapping to the correct native instruction.

### Naming and Organization

| Category | Convention | Example |
|----------|-----------|---------|
| Shader entry points | PascalCase, prefixed with stage | `VertexMain`, `FragmentMain`, `ComputeMain` |
| Types (struct, interface) | PascalCase | `SceneData`, `ILight`, `PBRMaterial` |
| Functions | camelCase | `evaluateBSDF`, `sampleShadowMap` |
| Variables | camelCase | `worldPos`, `viewDir`, `lightCount` |
| File-scope resources (`parameterBlock`, `pushConstant`) | `g_` prefix | `g_scene`, `g_material` |
| Constants / `static const` | `k` prefix + PascalCase | `kMaxLights`, `kPi` |
| Generic type parameters | Single uppercase letter or PascalCase with `T` prefix | `T`, `TLight` |

### Project Conventions

- All shader source files reside in `Engine/Shaders/` (mirrored by the engine's shader-loader search path).
- Test shaders live in `Engine/Shaders/Test/`.
- Each shader module that reflects into a C++ RHI binding should declare its expected resource counts via `static_assert`:

```hlsl
static_assert(kMaxLights <= 32, "Light count must fit in a uint32 bitmask");
```

- Use `[[vk::binding(N, set)]]` when Vulkan-specific explicit binding is required for interop with the engine's RHI descriptor layout. Keep it in a dedicated `__target_switch` block or `#ifdef __SPIRV__` guard.

### Quick Reference

| Rule | Scope | Rationale |
|------|-------|-----------|
| No `cbuffer` / `tbuffer` | All shaders | Use struct + ParameterBlock |
| No `register(...)` | All shaders | Auto-layout; use `[[binding]]` when explicit |
| Use colon-syntax semantics on struct members | Vertex inputs, varyings | Standard Slang/HLSL idiom |
| `public` for cross-module API | Module boundaries | Explicit contract; default is internal |
| `let` prefix on generic value params | Generic functions | Slang-required syntax |
| `interface` over preprocessor | Specialization | Type-safe, combinatorial |
| `__target_switch` over `#ifdef` target | Cross-target code | Cleaner, optimizable |
| Module: `module` / `implementing` / `import` | Multi-file shaders | Separate compilation |
| No `/* */` block comments | All shaders | Lexical-depth interaction with `__include` |
| `[[unroll]]` / `[ForceInline]` only when measured | Performance | Avoid speculative scattering |
| `g_` prefix on file-scope resources | All shaders | Distinguish from locals |
| `static_assert` for compile-time invariants | Resource limits, alignment | Fail fast at compile time |
