# Context: Material

**Namespace:** `SoulEngine::Material`

Runtime material domain model. It owns material records, their cross-renderer
shader-storage ABI, resolved texture resource records, and their future
template/instance vocabulary; it does not own scene serialization, shader
compilation, or backend-specific Vulkan bindings.

## Terms

| Term | Definition |
|------|------------|
| **Material** | Runtime description of surface appearance. It contains imported CPU values and resource-backed texture records. |
| **MaterialRecord** | Runtime resource record projected from Assimp's `aiMaterial`: common scalar/color properties, PBR extensions, and texture slots indexed by `aiTextureType` and slot index. |
| **TextureRecord** | Runtime representation of one Assimp texture slot, including source metadata and its `RHIRef<RHISampledTexture>`. |
| **MaterialRecord::GpuData** | Cross-renderer storage-buffer ABI containing resolved PBR factors and logical texture-table indices, but never GPU handles or descriptor state. |
| **Material Instance** | A named or referenced use of a material value. The legacy flat `MaterialManager` ID table was deleted on 2026-08-27; instances migrate to stable `MaterialHandle` cache identities (scene identity + instance name, or mesh asset path + slot index). |
| **Material Template** | Future shared definition of shader/pass interfaces and default values from which material instances override data. It is not represented in V0 because the only supported template is implicit PBR metallic-roughness. |

## Ownership and Dependencies

- `Material` depends on `Core`, `RHI`, EnTT, Assimp, and image decoding support;
  it must not depend on `Scene`, `Resource`, `Renderer`, or backend modules.
- `MaterialYamlRecord` owns YAML authoring data and Assets-relative texture paths.
- `MaterialAssimpLoader` imports Assimp materials; `MaterialYamlLoader` imports
  complete material YAML records. Both produce `MaterialRecord` handles.
- Mesh import registers imported materials through `MeshLoader`'s
  `MaterialAssimpCache`; `SubMesh` carries the resulting `MaterialHandle`.
- `Renderer` captures material records once per frame, resolves each draw's material dependencies, and builds its own GPU parameter snapshots.
- `RHI` remains material-agnostic; it only exposes shader parameters and resource binding primitives.

## Evolution

`Material:AssimpLoader` imports one Assimp `aiMaterial` into one cached
`MaterialRecord`. Its cache key is formed by the normalized model path,
material name, and material index; the index disambiguates empty or duplicate
Assimp names. It owns the `TextureDataCache` used by its texture slots and
does not assign material IDs, build GPU data, or publish descriptors.
`MaterialRecord::Textures[type][index]` preserves Assimp's texture semantic
and per-semantic slot index, including multi-slot types such as clearcoat,
sheen, and transmission.

Add typed import normalization, templates, and material-instance overrides here
when their ownership becomes concrete. Keep scene serialization, GPU texture
resolution, descriptor publication, and renderer frame caches outside this
module.

`Material:YamlLoader` loads a complete `material` YAML mapping into a
`MaterialYamlRecord`, resolves its texture paths against the application
`Assets` root, and creates a `MaterialRecord`. It owns an independent
`TextureDataCache` for now; sharing this cache with the Assimp path is a future
optimization.
