# Context: ShaderCompiler

**Namespace:** `SoulEngine`

Shader compilation pipeline with a backend-per-language architecture.
The `ShaderCompiler` facade dispatches pipeline compile requests to internal
per-language backends. Backend selection is explicit in each shader entry; file
extensions are warning-only validation.

Graphics pipelines use `GraphicsCompileDesc`, which composes and links the
requested vertex and fragment entry points in one compiler request so reflection
describes the final pipeline shader interface.

## Terms

| Term | Definition |
|------|------------|
| **ShaderCompiler** | Public singleton facade. Entry point for pipeline shader compilation. Routes compile requests to the correct backend. |
| **ShaderEntry** | Request for one shader entry point in a source file. Pipeline compile descriptors compose these entries. |
| **GraphicsCompileDesc** | Compile request descriptor for one graphics pipeline shader combination. It names the vertex and fragment entry points and include directories. |
| **GraphicsProgram** | Shared Shader artifact produced by ShaderCompiler for graphics pipelines. It contains one linked SPIR-V code blob, vertex/fragment entry-point names, and linked pipeline-level reflection. Refers to `ShaderGraphicsProgram`. |
| **IBackend** | Abstract interface for per-language compiler backends. Module-private. |
| **SlangCompiler** | Concrete Slang backend, in the `SlangBackend` class. Translates `.slang` sources to SPIR-V via the Slang SDK. Implemented in the `SlangCompiler/` directory as module partitions `Slang:Types`, `Slang:Utils`, and `Slang:Reflection`, all within `SoulEngine`. |
| **Backend** | Enum of supported backend languages. Currently only `Slang`. |
| **Reflection DFS** | The `Slang:Reflection` partition collects the whole program interface in one depth-first walk over the Slang variable tree (`VisitVariable`). Two orthogonal dimensions meet per node: location coordinates accumulate in a `ReflectionCursor` along the path (binding space -> set, binding index -> descriptor slot, Uniform/PushConstantBuffer categories -> push-constant byte offsets), while the type-layout kind drives recursion (ParameterBlock opens a new set with slot zero, plain structs are resource facades to descend into, resource leaves emit bindings). Slang reports every variable's space/slot relative to its enclosing scope, so absolute locations come from cursor accumulation; a ParameterBlock variable reports its own set through its binding index. |

## Dependencies

- `Core` — logging, config
- `Shader` — `ShaderStage`, `ShaderGraphicsProgram`
- Third-party: `vulkansdk` (Slang SDK headers: `slang.h`, `slang-com-ptr.h`)

## Relationships

- `ShaderCompiler::CompileGraphics()` is the production path for graphics pipeline shader requests. It asks the backend to compose/link the requested stages and return one `ShaderGraphicsProgram` with pipeline-level reflection.
- Pipeline compile results take canonical entry-point names and stages from Slang reflection metadata.
- Slang compilation failure, missing modules, invalid entry points, and linked reflection failures are distinct failure cases.

## Test fixture containment

Shader-compiler tests must only compile self-contained fixtures under
`Tests/Slang/`. They must never load runtime engine or application shaders
(`Engine/Shaders/`, `Applications/`): runtime shaders move, split into
modules, and change with renderer work, which breaks tests for unrelated
reasons. Fixtures model one compiler capability at a time and must not mirror
the resource surface or entry-point set of a runtime program.

## Ray-tracing BDA target capability

Ray-tracing Slang sessions enable both `spvRayTracingKHR` and
`SPV_EXT_physical_storage_buffer`. Physical pointer syntax is confined to the
small ray-tracing metadata lookup abstraction; ordinary runtime shaders retain
backend-neutral reflected resource declarations. The fixed metadata
`ByteAddressBuffer` must remain reflectable as a normal storage-buffer binding.
