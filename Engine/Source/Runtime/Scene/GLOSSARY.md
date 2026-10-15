# Scene

Mutable world-state container owned by Application, read by Renderer through an immutable per-frame snapshot. Scene authoring components are renderer-neutral and hold no RHI resources; ref-backed geometry payloads on GeometryRecord are the explicit exception. The Scene Document / Runtime World split is ADR 08.

## Language

### Documents and worlds

**Scene**:
Concrete mutable runtime world owned by Application — entities, transforms, meshes, materials, lights, and other entity-associated state.

**Scene Document**:
Author-facing YAML description of one scene — the interchange boundary for human and AI editing, not the runtime source of truth (ADR 08).

**Runtime World**:
The mutable in-memory state built from a Scene Document; the source from which render snapshots are built.

**Scene Replacement**:
Loading constructs a complete temporary Runtime World and atomically replaces the current Scene only after all structural data is valid — never a merge or patch.

**Structural Error / Component Warning**:
The two document-diagnostic classes: a structural error (invalid tree or mandatory data) rejects the whole file; a component warning (unknown or invalid optional component) is logged, that component is omitted, and the rest of the scene loads.

**Authoring State / Runtime State**:
Authoring state is what a Scene Document persists and people or agents edit; runtime state is ephemeral and never persisted. Component-private runtime state may live beside its authoring state; shared or renderer-owned state lives elsewhere.

### Entities and hierarchy

**Scene Registry**:
The ECS registry owned by a Scene; it contains Scene Entities only — non-spatial runtime concerns do not belong in it.

**Scene Entity / Scene Node**:
A runtime entity in the registry, and its mandatory structural record: every Scene Entity has exactly one Scene Node and is therefore spatial. The node owns the parent-child relationship, child order, and derived world matrix.

**Scene Hierarchy**:
The ordered, acyclic forest of entity parent-child relationships — general organisation and inheritance, not only transforms.

**World Coordinate System**:
Right-handed, Y-up. Asset-format coordinate differences are converted at the asset-import boundary.

**Component Map**:
The per-entity mapping of component kind to authoring data; an entity has at most one component of each kind, and omitted components receive runtime defaults.

**Component Type Naming**:
Optional ECS component types use the `Component` suffix (CameraComponent, LightComponent, MeshComponent).

### Components and records

**CameraComponent**:
Optional component describing a camera: lens parameters plus the Viewport rectangle. Holds no RHI resources; control behaviour is separate runtime state.

**LightComponent**:
Optional authoring component describing a directional or point light. Spot lights are not part of the current contract.

**MeshComponent**:
Optional component associating an entity with mesh content — an asset path and an optional material override path, relative to the application assets directory. GPU representations are renderer-owned, never component state.

**Viewport**:
A physical-pixel rectangle on the window where a camera's final image lands; persisted by CameraComponent, carried by resize events, targeted by the present blit.

**PixelCoordinate**:
A physical framebuffer pixel coordinate carried as optional editor selection input; the renderer post-process reads the entity-id target at this coordinate.

**SceneSnapshot**:
Immutable per-frame render view built at the end of the game loop: camera views, value-semantic instance records, and optional editor selection input (ADR 06).

**CameraViewRecord**:
One immutable camera/view record: view-projection data plus the view's Viewport. Carries no RHI resources — view render targets are renderer-created pooled graph transients (ADR 05).

**InstanceRecord**:
Value-semantic snapshot record for one geometry/material instance: entity id, geometry handle, selected material handle, derived world transform.

**GeometryRecord**:
Scene-owned imported geometry record: CPU vertex metadata, local bounding sphere, and ref-backed vertex/index buffers with an explicit GPU-address ABI. It has no material identity, and bitangents are not stored (consumers derive them).

**SubMesh / MeshAssetNode / MeshRecord**:
The imported-mesh vocabulary: a SubMesh is one imported sub-geometry with its default material binding; a MeshAssetNode is one node of an imported asset hierarchy referencing SubMeshes; a MeshRecord is the Scene-local cached asset owning the material table, deduplicated SubMeshes, and the immutable node tree shared by every instancing entity.

**LightRecord**:
Immutable world-space light record in a snapshot — source entity id, physical light values, and the shared GPU ABI consumed by both renderers.

**Scene system query**:
Templated lookup of one registered Scene system, returning an optional borrowed reference while hiding the scheduler from consumers.
