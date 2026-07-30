# Context: Material

**Namespace:** `SoulEngine::Material`

Renderer-neutral material domain model. It owns material value types, their cross-renderer shader-storage ABI, and their
future template/instance vocabulary; it does not own scene registration, asset
loading, GPU handles, shader compilation, or Vulkan bindings.

## Terms

| Term | Definition |
|------|------------|
| **Material** | Renderer-neutral description of surface appearance. It contains authoring/runtime CPU values, never backend handles. |
| **PbrMetallicRoughnessMaterial** | Current built-in material value. It contains base-color, metallic, roughness, and emissive factors plus optional renderer-neutral texture asset references for the V0 PBR maps. |
| **PbrMaterialGpuData** | Cross-renderer storage-buffer ABI containing resolved PBR factors and logical texture-table indices, but never GPU handles or descriptor state. |
| **Material Instance** | A named or referenced use of a material value. V0 IDs and lifetime are scene-owned: a Scene Document maps IDs in `material_instances` to `PbrMetallicRoughnessMaterial` values. |
| **Material Template** | Future shared definition of shader/pass interfaces and default values from which material instances override data. It is not represented in V0 because the only supported template is implicit PBR metallic-roughness. |

## Ownership and Dependencies

- `Material` depends only on `Core` and `hlsl++`.
- `Material` must not depend on `Scene`, `Resource`, `Renderer`, `RHI`, or backend modules.
- `Scene` currently owns scene-local material-instance ID registration and YAML persistence.
- `Renderer` consumes material values from a `SceneSnapshot` and creates its own GPU parameter snapshots.
- `RHI` remains material-agnostic; it only exposes shader parameters and resource binding primitives.

## Evolution

Add texture references, templates, typed parameter layouts, and material-instance
overrides here when their ownership becomes concrete. Keep scene serialization and
renderer GPU caches outside this module unless they become general asset services.
