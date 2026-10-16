# Shader

Shader data model — the types describing compiled shader artifacts. This module is the data contract between ShaderCompiler (produces) and RHI (consumes), and is deliberately independent of both.

## Language

**Stage**:
Canonical pipeline stage enum (Vertex, Fragment, Compute, and the rest). Compiler, RHI, and reflection all speak it; backends map to and from their native stage types.
_Avoid_: RHIStage (a deleted 3-value subset), SlangStage (backend-native, mapped only inside the Slang backend)

**GraphicsProgram**:
Compiled artifact for one graphics pipeline shader combination: one linked SPIR-V blob, the vertex/fragment entry-point names inside it, and the linked pipeline-level reflection.

**Reflection**:
Normalized pipeline reflection data — bindings, push constants, vertex inputs, specialization constants — owned by value by the GraphicsProgram.

**Binding**:
Reflected shader-visible resource binding (parameter path, set, binding, type, array count). The parameter path is the stable host-side lookup key within one pipeline layout.

**Shader binding name**:
Canonical host-side name of one reflected binding within one linked pipeline layout — currently the binding's parameter path. Never conflate it with an asset name, an asset cache key, or a backend descriptor object name.
_Avoid_: asset name, descriptor name

**ResourceType**:
Minimal resource categories (ConstantBuffer, StorageBuffer, Texture, Sampler, AccelerationStructure); the axis splits only where backend descriptor types differ, with access direction riding the separate BindingAccess axis.

**BindingAccess**:
Reflected shader-declared access direction of a binding (Read / ReadWrite), sourced from the shader's own declaration and independent of ResourceType.

**SpecializationConstant**:
Reflected specialization constant (name, constant id, scalar type, optional default). Host-provided values are fixed at pipeline creation and become part of the pipeline identity; omitted constants keep their shader-declared defaults.

**ScalarType / ValueType**:
Minimal reflected scalar vocabulary (Float32, Int32, Uint32, Bool) and the scalar/vector/matrix shape built from it.

**PushConstantRange**:
Reflected push-constant byte range (offset, size).

**VertexInputAttribute**:
Reflected vertex attribute requirement (semantic, location, value type) — what the shader consumes, not how CPU-side vertex buffers feed it.

**Runtime constant-buffer ABI**:
Runtime constant buffers use the default std140 layout. Shader structs declare semantic fields only — never named padding — and CPU mirrors lock reflected offsets with sizeof/offsetof assertions. Reflection or RenderDoc is authoritative when an assumed layout disagrees with observed SPIR-V.

**LightRecord**:
Shared storage-buffer ABI for directional and point lights; the light type rides an explicit shader-side constant.
