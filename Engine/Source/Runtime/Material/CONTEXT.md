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
| **MaterialRecord** | Runtime resource record projected from Assimp or Material YAML: loader provenance (`MaterialSource` and normalized source asset), common scalar/color properties, PBR extensions, and an ordered flat collection of `TextureRecord` values. |
| **ConstMaterialHandle** | Read-only `entt::resource<const MaterialRecord>` used by inspection queries. Runtime loading, material overrides, and render bindings retain mutable `MaterialHandle` values. |
| **MaterialSource** | The loader family that created a MaterialRecord: YAML or Assimp. It is independent of the material's display name, which may be duplicated; loaders normalize an empty source name to `<unnamed>`. |
| **MaterialAlphaMode** | Alpha coverage policy: `Opaque`, `Mask`, or `Blend`. `Unknown` represents an unrecognised imported value; normal YAML and glTF defaults are `Opaque`. |
| **AlphaCutoff** | Threshold used by `MaterialAlphaMode::Mask`; it defaults to `0.5`. |
| **TextureType** | Material-owned texture semantic. Assimp types are normalized to this enum at the Assimp import boundary; YAML loading creates these semantics directly. |
| **TextureRecord** | Runtime representation of one material texture slot, including its `TextureType`, source metadata, and its `RHIRef<RHISampledTexture>`. |
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
`MaterialRecord`. Its cache key is formed by the normalized model path and
material index, which disambiguates duplicate names. It owns the
`TextureDataCache` used by its texture slots and
does not assign material IDs, build GPU data, or publish descriptors.
`MaterialRecord::Textures` preserves imported order, while every
`TextureRecord::Type` carries its Material-owned semantic. This preserves
multi-slot types such as clearcoat, sheen, and transmission without exposing
Assimp enumeration types to consumers.

Add typed import normalization, templates, and material-instance overrides here
when their ownership becomes concrete. Keep scene serialization, GPU texture
resolution, descriptor publication, and renderer frame caches outside this
module.

`Material:YamlLoader` loads a complete `material` YAML mapping into a
`MaterialYamlRecord`, resolves its texture paths against the application
`Assets` root, and creates a `MaterialRecord`. It owns an independent
`TextureDataCache` for now; sharing this cache with the Assimp path is a future
optimization.

`MaterialAlphaMode` and `AlphaCutoff` are currently imported from glTF,
authorable through material YAML, and visible to Editor inspection only. The
renderer does not yet use them for alpha testing, blending, or pass selection.
