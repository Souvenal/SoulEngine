# ADR 08 — YAML Scene Documents and EnTT Runtime Worlds

## Status

Accepted (2026-07-20)

## Context

SoulEngine needs an editable scene representation and an ECS-based runtime
world. During early development, scenes must remain readable and directly
editable by people and AI agents. YAML is suitable for that authoring boundary,
but is deliberately not assumed to be the engine's permanent persistence
format.

The existing Scene prototype stores cameras and mesh resource references
directly. It cannot represent a general entity hierarchy, does not distinguish
authoring data from runtime data, and cannot instance one mesh resource at
multiple world transforms. ADR 06 already establishes the mutable Scene to
immutable SceneSnapshot handoff; this ADR refines the mutable Scene model while
retaining that handoff.

## Decision

- A Scene Document is one YAML Scene File containing one scene. V1 uses a pure,
  nested entity tree rooted at a top-level entity list. It has no explicit
  schema version, persisted entity UUIDs, cross-entity references, merge
  semantics, or patch semantics.
- Scene Files use the YAML mapping, sequence, and scalar subset only. Anchors,
  aliases, explicit tags, directives, multiple documents, and duplicate mapping
  keys are rejected as Structural Errors.
- YAML is read-only authoring input. Loading builds a temporary Runtime World and
  atomically replaces the active Scene once required structure is valid.
  Structural errors reject the file. Unknown or invalid optional components
  produce warnings and are omitted while the rest of the scene loads.
- Scene owns an EnTT Scene Registry. It contains only spatial Scene Entities.
  Every Scene Entity has exactly one mandatory Scene Node, which owns ordered
  hierarchy links and a derived world matrix. Local transform authoring is
  represented by the optional `components.transform` component; runtime entity
  creation always supplies a default `TransformComponent`.
  The world is right-handed and Y-up; document rotations are Euler angles in
  degrees, applied in local X → Y → Z order.
- ECS types use the `XxxComponent` naming convention and are represented under
  the entity's `components` mapping using their registered EnTT meta names.
  Components may contain both document-loaded authoring fields and
  component-private runtime fields. `TransformComponent`, `CameraComponent`,
  `MeshComponent`, and `LightComponent` are the component model. A
  `MeshComponent` persists a project-relative mesh asset path. Renderer-specific
  mesh resource references, uploads, and GPU representations are renderer-owned.
- `entt::meta` is the single registration source for component document names
  and YAML-writable fields. Each built-in component statically registers its
  type and explicit `meta_data` fields beside its definition. Scene loading
  directly emplaces and removes the built-in component types.
- `Scene` owns shared Scene model types. Each component family owns one
  `Scene:<Name>` partition and its static metadata registration. `Scene:YamlIO`
  resolves names and applies metadata, then directly emplaces the built-in
  component types.
- YAML loading stays inside the Scene module in a non-exported IO
  partition. `libyaml` implementation types do not cross the public Scene API.
- `Resource::Mesh` exposes imported mesh groups and submeshes with their
  resource handles. Each renderer expands ready submeshes from its own mesh-resource
  cache into renderer-local draw instances. SceneSnapshot carries value-semantic
  Renderable Instances, which pair a mesh asset identity with a Scene Entity's
  derived world Transform and no GPU draw representation.

## Consequences

YAML can be replaced later by changing the non-public loading partition
without changing the Runtime World or renderer-facing snapshot contract. The
initial format is intentionally simple and AI-friendly, but cannot yet express
entity references, prefabs, incremental scene updates, or robust document
merging; those features require persistent document identity and a later format
evolution.

Keeping component-private runtime data beside authoring data avoids paired
component synchronization while preserving a strict document-input boundary.
Omitted document components retain their runtime defaults, including the
default transform created for every entity.
The renderer continues to consume immutable snapshots. Renderer-owned mesh
resource caches retain resource ownership while the renderer resolves asset
identities from those snapshots.

## Considered Options

- Put YAML support in a separate `SceneIO` module. Rejected for V1: a
  non-exported Scene IO partition provides the same public API isolation with a
  smaller module surface.
- Persist UUIDs for every entity immediately. Rejected for V1: a pure nested
  tree needs no document identity until cross-entity references, prefabs, or
  patching are introduced.
- Use separate authoring and runtime ECS components. Rejected as a default:
  one component may safely co-locate both when meta persists only authoring
  fields.
- Add world transforms to Resource mesh submeshes. Rejected because a resource
  asset may be instantiated by multiple Scene Entities at different transforms.

## Non-Goals

This ADR does not define light rendering, materials, prefabs, cross-scene
references, editor tooling, or a future Scene Document versioning and migration
scheme.
