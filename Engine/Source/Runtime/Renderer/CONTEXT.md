# Context: Renderer

**Namespace:** SoulEngine::Renderer

Render pipeline orchestration layer — drives RHI command execution and pass composition.

## Terms

| Term | Definition |
|------|------------|
| **IRenderer** | Abstract base class for all renderers. Defines OnAttach(), OnDetach(), Render(const Scene::SceneSnapshot&) -> expected<CommandList, ErrorMessage>. Returns a command list — does NOT touch BeginFrame/EndFrame. |
| **ForwardRenderer** | Current forward PBR renderer. Owns asynchronous pipeline, sampler, texture, and logical frame/view/material constant-buffer refs, resolves ready Mesh submeshes into forward draw instances, and renders direct directional light with the shared metallic-roughness BRDF and each draw scene-authored material values. Draws may bind an optional scene-authored base-color texture through the bindless `g_textures`/`g_samplers` sampling blocks. |
| **Constant-buffer ABI mirror** | Renderer-side C++ mirror for a Slang constant-buffer struct. It contains semantic fields only, uses lignas(16) for required members, and proves the reflected std140 offsets with sizeof and offsetof assertions. |
| **Pass dependency** | Resource dependency whose absence makes an entire pass incoherent for the current frame. |
| **Draw dependency** | Resource dependency whose absence affects one draw instance or material use, not necessarily the entire pass. |
| **Draw material data** | Per-draw or per-material shader data that selects resources or parameters for one draw instance. It names shader bindings by reflected parameter path, never by backend set or binding number. |
| **RayTracingRenderer** | Progressive hardware path tracer. It uses TLAS custom indices and a fixed BDA geometry metadata table to select original mesh position, normal, and index buffers, traces direct-light shadow rays and BSDF bounces, and accumulates one sample per pixel per frame until the scene or view changes. |

## Architecture

`
IRenderer
 ├── ForwardRenderer  (current — direct-light PBR)
 ├── RayTracingRenderer (current — progressive metallic-roughness path tracer)
 └── DeferredRenderer (future)
`

## Dependencies

- Core — logging, config
- RHI — RenderDevice::Get(), CommandList, pipeline types
- Scene — SceneSnapshot read-only per-frame render input
- Resource — async texture, buffer, and pipeline requests

## Relationships

- **Application** owns UPtr<IRenderer>, creates it in OnAttach(), and provides mutable scene state. GameLoop builds SceneSnapshot for RenderLoop. Application no longer calls BeginFrame/EndFrame — those moved to RenderDevice::Execute() on RHIThread.
- **EngineLoop** owns the frame pipeline: GameLoop calls OnTick(), RenderLoop calls Renderer::Render() -> gets back CommandList, RHILoop calls RenderDevice::Execute(CommandList).
- Pipeline, texture, and buffer lifetime flow through ResourceManager refs; sync resource creation is not done in renderer attach or render-frame code.
- Resource readiness is resolved before emitting coherent RHI commands. Pass dependencies decide whether a pass is emitted; draw dependencies decide whether an individual draw is emitted, skipped, or substituted with fallback resources.
- Renderers must render normal color output into explicit render targets and set CommandList::PresentSource for window presentation. They must not rely on null color attachments as an implicit swapchain target.
- Renderer creates RHIShaderParameters from a ready pipeline reflection-derived layout, assigns values by shader parameter path, and binds the resulting snapshot. Each simultaneous render view needs a stable parameter snapshot identity so Vulkan gives it a distinct descriptor-set instance. Renderer code must not know Vulkan set or binding numbers.
- Bindless texture selection is **Draw material data**, not a shader entry-point identity or pipeline compile-time interface. Texture tables are assigned as shader parameter values; material/object data selects entries by integer texture indices.
- SceneSnapshot carries renderer-neutral RenderableInstance values, not Resource-level draw representations. Each renderer owns a mesh-resource cache keyed by mesh asset identity, requests meshes incrementally, expands ready mesh submeshes into renderer-local draw instances, and skips work whose dependencies are not yet ready.
- ForwardRenderer binds position, generated normal, and UV vertex streams (meshes without UVs reuse the position stream as a placeholder binding). It uses an explicit directional-light frame block, per-view camera block, and a shared per-draw object/material constant block. Scene-authored base-color textures are requested through a renderer-owned texture cache and assigned per draw via `g_textures.uTextures` element 0 with the draw material's integer texture index; the view-level parameter snapshot binds both `g_samplers` samplers and a null texture slot so every reflected binding always carries a value. Its shader and C++ constant-buffer mirrors follow the project runtime std140 ABI: no named padding fields, lignas(16) only where the shader type requires it, and sizeof/offsetof assertions for every reflected field offset.
- RayTracingRenderer keeps material ownership in Scene/Material and uses a renderer-local material table. For each dispatch it builds a logical geometry-table update from ready Resource Mesh submeshes, binds only g_rayTracingGeometryMetadata.metadata, and never handles Vulkan device addresses or descriptor indices. RHI resolves the original source-buffer addresses and ensures their lifetime and visibility for BDA ray-hit shading.