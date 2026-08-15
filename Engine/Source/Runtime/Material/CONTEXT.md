# Context: Material

**Namespace:** `SoulEngine::Material`

Renderer-neutral material domain model. It owns material value types, their cross-renderer shader-storage ABI, the
engine-wide pure-CPU material instance registry, and their future template/instance
vocabulary; it does not own scene serialization, asset loading, GPU handles,
shader compilation, or Vulkan bindings.

## Terms

| Term | Definition |
|------|------------|
| **Material** | Renderer-neutral description of surface appearance. It contains authoring/runtime CPU values, never backend handles. |
| **PbrMaterial** | Current built-in material value. It contains base-color, metallic, roughness, and emissive factors plus optional renderer-neutral texture asset references for the V0 PBR maps. |
| **PbrMaterialGpuData** | Cross-renderer storage-buffer ABI containing resolved PBR factors and logical texture-table indices, but never GPU handles or descriptor state. |
| **Material Instance** | A named or referenced use of a material value. V0 IDs live in one flat `MaterialManager` table: scene documents use their `material_instances` names, mesh import uses `"<mesh asset identity>#<material slot>"` IDs built at the call site. |
| **MaterialManager** | Engine-wide singleton pure-CPU registry (`MaterialManager` module, in Resource target). One flat `std::vector<MaterialEntry>` table; `RegisterMaterial(Name, Value)` appends or overwrites in place; `Clear()` drops everything (called by Scene replacement before registering the new scene); `GetMaterials()` returns a small value copy for the render thread; `FindMaterial()` is a free linear lookup. Manages texture references internally. |
| **Material Template** | Future shared definition of shader/pass interfaces and default values from which material instances override data. It is not represented in V0 because the only supported template is implicit PBR metallic-roughness. |

## Ownership and Dependencies

- `Material` depends only on `Core` and `hlsl++`; `MaterialManager` (same target) additionally uses `std::mutex`/`SPtr`.
- `Material` must not depend on `Scene`, `Resource`, `Renderer`, `RHI`, or backend modules.
- `Scene` owns the authoring store (`material_instances` YAML, `SetMaterialInstance`) and, on a successful load, clears `MaterialManager` then registers resolved values (scene replacement never inherits stale instances).
- `Resource` registers mesh-imported materials into `MaterialManager` during async mesh import (`SubMesh::MaterialId`).
- `Renderer` captures the material table (`GetMaterials()`) once per frame, resolves each draw's instance chain (scene -> asset -> default) via `FindMaterial`, and builds its own GPU parameter snapshots.
- `RHI` remains material-agnostic; it only exposes shader parameters and resource binding primitives.

## Evolution

Add texture references, templates, typed parameter layouts, and material-instance
overrides here when their ownership becomes concrete. Keep scene serialization and
renderer GPU caches outside this module unless they become general asset services.