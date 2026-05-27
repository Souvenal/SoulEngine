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
Minimal resource categories (`UniformBuffer`, `StorageBuffer`, `SampledTexture`, `StorageTexture`, `Sampler`).

**ValueType**:
Reflected scalar/vector/matrix shape (scalar type, rows, columns).

**Binding**:
Reflected shader-visible resource binding (set, binding, type, array count).

**PushConstantRange**:
Reflected push-constant byte range (offset, size).

**VertexInputAttribute**:
Reflected vertex attribute requirement (semantic name/index, location, value type). Describes what the shader consumes, not how CPU-side vertex buffers feed it.

**Reflection**:
Normalized per-program reflection data (bindings, push constants, vertex inputs).

**Program**:
Compiled shader artifact for one pipeline stage (code blob, entry-point name, stage, reflection). Produced by ShaderCompiler, consumed by RHI.

## Relationships

- A **Shader module** type (`Stage`, `Program`, `Reflection`) is defined in `Shader`, referenced by both `ShaderCompiler` and `RHI`.
- **ShaderCompiler** produces **Program** values through per-language backends; `Shader` itself has no compiler dependency.
- **RHI** consumes **Program** values directly when constructing pipelines and merges their **Reflection** into a pipeline-level **PipelineResourceLayout**.
- Backends (Slang → `ToShaderStage`, Vulkan → `ToVkShaderStage`) map between `Stage` and their native stage enums.

## Example dialogue

> **Dev:** "I need to add a new pipeline stage. Do I touch `Stage`?"
> **Domain expert:** "Yes — that's the canonical stage enum. Every consumer (ShaderCompiler, RHI, backends) maps from it. Adding a stage there and updating the backend mapping functions is the procedure."

## Flagged ambiguities

- `RHIStage` was a 3-value subset that duplicated `Stage` — removed in ADR 0001. All code now uses `Shader::Stage`.
- `CompileDesc` lives in ShaderCompiler because it describes a compiler request, not a shared shader artifact.
