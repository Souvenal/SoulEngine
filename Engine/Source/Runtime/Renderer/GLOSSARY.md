# Renderer

Renderer implementations convert renderer-neutral SceneSnapshot input into a move-only RHI command list. Current renderers are Raster and RayTracing.

## Language

### Frame flow

**IRenderer**:
Renderer interface invoked by RenderLoop with one SceneSnapshot; returns the frame's render result.

**RenderResult**:
Per-frame RHI packet produced by RenderLoop and borrowed by RHILoop for recording; owned by the frame slot until GPU completion (ADR 03).

**View targets**:
The per-view render targets a renderer creates from the view's viewport: the GBuffer set and SceneColor for Raster, output/entity-id targets for RayTracing. Contents live only for the frame; cross-frame state (accumulation, future history) is a renderer-owned persistent resource imported per frame.

**GBuffer**:
Raster view-target set — four color targets (albedo, normal, material ID, entity ID) plus one shared depth target. Not a Scene concept; RayTracing produces no GBuffer.

**Geometry pass / Lighting pass**:
The raster pass pair — geometry writes the GBuffer, lighting samples it and writes SceneColor.

**Post-process pass**:
A raster pass appended after scene lighting, operating on the current view's targets.

**EditorViewRecord**:
Editor-only view snapshot carrying one editor camera pose and viewport; its view targets are pooled transients like any other view's.

**Editor selection outline**:
Editor post-process that reads the entity-id target at the selected pixel and outlines adjacent pixels sharing the selected id.

### Geometry and instances

**SceneGpuData**:
Per-frame renderer-neutral GPU table bundle (instances, geometry, materials, lights) built from renderer-filtered instance records and the full light list. Instances are sorted by geometry id as the canonical order; lights bake once per frame shared by every view; material resolves through the instance record, never through the geometry.

**Lazy upload**:
Geometry buffers (and BLAS) are created on first encounter by a renderer, not at asset load. Creation is synchronous, so the requesting frame bakes real device addresses and the bytes land in the same frame's upload batch — the record is drawable from the requesting frame.

**GeometryUploader**:
Per-renderer sole authority to mutate a geometry record's GPU state through one idempotent entry point; queries live on the record, and mutation is never a member operation.

**Static BLAS**:
Bottom-level acceleration structure owned by a geometry record — built once, immutable for the record's lifetime (no update/refit; dynamic geometry is out of scope). Geometry ownership replaces any external key-based BLAS cache.

**Per-frame TLAS**:
Top-level acceleration structure created fresh each frame from the instance list, with no persistent backing (ADR 05). Its slot order is the canonical instance order — shaders map instance indices back through it.

**GeometryRecordTable**:
Shader-side facade over the per-pass geometry record table; parameter blocks embed it instead of a raw buffer, and it fully encapsulates the record's GPU-address ABI (vertex pulling).

**VertexInfo**:
Decoded vertex attributes (position, normal, tangent, uv) returned by the geometry table facade.
