# Context: Shader

**Namespace:** `SoulEngine::Shader`

Shader data model — owns the types that describe compiled shader artifacts.
This module is the data contract between ShaderCompiler (produces) and RHI (consumes).
It is deliberately independent of both: the types here describe *what a compiled shader is*,
not how it is compiled or bound.

## Terms

**Stage**:
Canonical pipeline stage enum (`Vertex`, `Fragment`, `Compute`, `Hull`, `Domain`, `Geometry`, `Mesh`, `Amplification`). Used by the compiler to describe what stage to compile for, by the RHI to select pipeline bind points, and by reflection to label entry points. Backends (Slang, Vulkan) map to/from their native stage types.
_Avoid_: RHIStage (deleted — was a 3-value subset), SlangStage (native Slang type, mapped in the Slang backend only).

**ScalarType**:
Minimal scalar-type vocabulary for normalized reflection (`Float32`, `Int32`, `Uint32`).

**ResourceType**:
Minimal resource categories (`ConstantBuffer`, `StorageBuffer`, `SampledTexture`, `StorageTexture`, `Sampler`).

**ValueType**:
Reflected scalar/vector/matrix shape (scalar type, rows, columns).

**Binding**:
Reflected shader-visible resource binding (parameter path, set, binding, type, array count).
The path is the full shader parameter path within the reflected program, such as
`g_frameView.view`, and is the stable host-side lookup key inside one pipeline
layout. It is not a Resource cache key or backend descriptor name.

**PushConstantRange**:
Reflected push-constant byte range (offset, size).

**VertexInputAttribute**:
Reflected vertex attribute requirement (semantic name/index, location, value type). Describes what the shader consumes, not how CPU-side vertex buffers feed it.

**Reflection**:
Normalized pipeline reflection data (bindings, push constants, vertex inputs).
`GraphicsProgram` owns the linked graphics shader combination's reflection by
value.

**GraphicsProgram**:
Compiled shader artifact for one graphics pipeline shader combination. It owns
one linked SPIR-V code blob, the vertex/fragment entry-point names inside that
blob, and the linked pipeline-level reflection.

## Relationships

- A **Shader module** type (`Stage`, `GraphicsProgram`, `Reflection`) is defined in `Shader`, referenced by both `ShaderCompiler` and `RHI`.
- **ShaderCompiler** produces **GraphicsProgram** values for graphics pipeline compile requests; `Shader` itself has no compiler dependency.
- **RHI** consumes **GraphicsProgram** values directly when constructing graphics pipelines. The pipeline-level **Reflection** is already part of the `GraphicsProgram`.
- Backends map between `Stage` and their native stage enums where stage metadata is still needed.

## Example dialogue

> **Dev:** "I need to add a new pipeline stage. Do I touch `Stage`?"
> **Domain expert:** "Yes — that's the canonical stage enum. Every consumer (ShaderCompiler, RHI, backends) maps from it. Adding a stage there and updating the backend mapping functions is the procedure."

## Flagged ambiguities

- `RHIStage` was a 3-value subset that duplicated `Stage` — removed in ADR 0001. All code now uses `Shader::Stage`.
