# Context: Scene

**Namespace:** `SoulEngine::Scene`

Mutable world-state container for runtime and editor logic. Owned by
Application, read by Renderer through a per-frame `SceneSnapshot`.

## Terms

| Term | Definition |
|------|------------|
| **Scene** | Concrete mutable runtime world owned by Application. Holds runtime/editor state, including logical cameras, meshes, materials, lights, transforms, and other entity-associated state. |
| **Scene Document** | Author-facing YAML input description of one scene. It is the interchange boundary for human and AI editing, rather than the runtime source of truth. |
| **Runtime World** | The mutable, in-memory state created from a Scene Document and used by runtime/editor systems. It is the source from which render snapshots are built. |
| **Scene Hierarchy** | The ordered, acyclic forest of entity parent-child relationships in a Scene. It represents general organisation and inheritance, not only transform relationships. |
| **Document Tree** | The initial Scene Document shape: a pure nested entity tree. It represents parent-child relationships structurally and contains no persisted entity identity or cross-entity references. |
| **Scene File** | One YAML file containing exactly one Scene Document. It may contain scene-local `material_instances` plus an `entities` root list for the Document Tree. The initial format omits an explicit schema-version field. |
| **Scene Entity** | A runtime entity represented in the Scene registry. Every Scene Entity has exactly one Scene Node and is therefore spatial. |
| **Scene Node** | The mandatory structural record for a Scene Entity. It owns the entity's `Core:Math` local Transform, derived world matrix, parent-child relationship, and child order. |
| **Scene Registry** | The ECS registry owned by a Scene. It contains Scene Entities only; non-spatial runtime concerns do not belong in it. |
| **Authoring State** | Scene data intentionally represented in a Scene Document and editable by people or AI agents. |
| **Component Map** | The `components` mapping on an entity node. Each key names one optional component kind and each value is that component's authoring data; a Scene Entity has at most one component of each kind. |
| **Component Type Naming** | Optional ECS component types use the C++ suffix `Component`, such as `CameraComponent`, `LightComponent`, and `MeshComponent`. Their Scene Document names are defined independently by EnTT meta type registration. |
| **MeshComponent** | Optional component that associates a Scene Entity with mesh content to be rendered. Its Authoring State is an asset path relative to the current application Assets directory, an optional scene-local `MaterialInstance` ID, and an optional base-color texture path (also Assets-relative). Renderer-specific mesh resource refs, uploads, and GPU representations are renderer-owned rather than component state. |
| **Structural Error** | A Scene Document error that prevents a valid Document Tree or mandatory Scene Node data from being constructed. It rejects the whole Scene File. |
| **Component Warning** | A recoverable issue in one optional component, including an unknown component, unknown field, or invalid component value. The loader warns and omits that component while loading the remainder of the Scene. |
| **Scene Loading** | Scene-owned, non-public YAML input implementation that constructs a Runtime World without exposing parser-specific types in the Scene API. |
| **Scene Replacement** | V1 loading constructs a complete temporary Runtime World and atomically replaces the current Scene only after all structural data is valid. It does not merge or patch an existing Scene. |
| **Runtime State** | Ephemeral state created while a Scene runs. It is not represented in a Scene Document. Component-private Runtime State may live beside that component's Authoring State; only shared or renderer-owned state must live elsewhere. |
| **World Coordinate System** | The Scene uses a right-handed, Y-up coordinate system. Asset-format coordinate differences are converted at an asset-import boundary. |
| **Transform** | `Core:Math` local translation, rotation, and scale data. Scene Documents express rotation as Euler angles in degrees, applied in local X → Y → Z order; Scene Node owns the world matrix derived through the Scene Hierarchy. |
| **SceneSnapshot** | Immutable per-frame render view built from `Scene` at the end of the GameLoop and held by the frame slot. It contains camera views, value-semantic `MeshInfo` records, and optional editor selection input. Material values are not part of the snapshot: the renderer resolves instance IDs through the engine-wide `MaterialManager`. Renderer consumes this snapshot, not the mutable `Scene`. |
| **RenderPixelCoordinate** | A physical framebuffer pixel coordinate carried as optional editor selection input. The renderer post-process reads the EntityId G-buffer at this coordinate to determine the selected ID. |
| **GBuffer** | Camera-owned ref-backed render-target set containing albedo, normal, material ID, entity ID, and one shared depth target. The depth target is both the geometry-pass depth attachment and the deferred lighting sampled depth resource. |
| **CameraRenderTargets** | Camera-owned output bundle containing the GBuffer and the final SceneColorRT render target. Post-process passes load SceneColorRT so they can overlay results without replacing the lighting image. |
| **RenderViewSnapshot** | One immutable camera/view record defined with the Camera component family: view-projection data plus ref-backed CameraRenderTargets. Renderers allocate their own transient constant buffers while recording the frame. |
| **MeshInfo** | Value-semantic SceneSnapshot record for one mesh entity. It carries the integer entity ID, normalized absolute mesh asset identity, scene-local material ID, and derived world transform, but no material payload or renderer-specific GPU resource. The material ID resolves through `MaterialManager` on the render thread. |
| **CameraComponent** | Optional component describing a camera attached to a Scene Entity. It persists only authoring camera data and may retain component-private runtime view state; control behaviour is separate Runtime State. |
| **LightComponent** | Optional authoring component describing a light attached to a Scene Entity. |

## Architecture

`Scene` is the mutable Runtime World owned by Application. Its Scene Registry
contains Scene Entities only, each with one mandatory Scene Node. Scene Nodes
form the ordered Document Tree hierarchy and derive world transforms from local
transforms.

`SceneSnapshot` is the immutable render-facing view copied into the frame slot
at the end of the GameLoop. The renderer consumes `SceneSnapshot` each frame
via `IRenderer::Render()`. Mesh instances are represented as Renderable Instances,
which pair an absolute asset identity with an entity-derived world transform.
Each renderer resolves that identity into its own draw-instance representation.
Material instances resolve through `MaterialManager`: scene load publishes
`material_instances`, mesh import publishes per-slot asset instances, and the
renderer applies the chain scene ID -> mesh-imported asset instance -> built-in
default, warning and falling through when a reference is missing.

`Scene` owns shared Scene model types. Each component family owns one
`Scene:<Name>` partition and its internal static EnTT meta registration.
`Scene:YamlIO` resolves component and field names through this metadata, then
directly emplaces the built-in component types.

Scene Loading is an internal Scene implementation. It uses a single-document
YAML subset of mappings, sequences, and scalars without exposing parser-specific
types through the Scene API. Anchors, aliases, explicit tags, directives, and
duplicate mapping keys are Structural Errors. V1 loads a complete replacement
Runtime World, reports Structural Errors for invalid required tree data, and
reports Component Warnings while omitting only invalid optional components.

Authoring State and component-private Runtime State may coexist in one
component type. Each component's explicitly registered EnTT meta data fields
participate in Scene Document loading. Renderer-owned resource caches retain asset-resource ownership while resolving the
asset identities in snapshots. RenderViewSnapshot additionally carries the camera-owned
RHIRef render-target bundle needed by the renderer; it carries no native raw pointers.
## Dependencies

- `Core` — types, error handling
- `Resource` — typed runtime resource refs and snapshot handles
- `entt` — Scene Registry and component metadata
- `libyaml` — internal Scene Loading implementation
- `hlsl++` — vector and matrix math

## RHI ownership boundary

Scene authoring components remain renderer-neutral and must not store backend raw
pointers. Camera-owned CameraRenderTargets are the explicit runtime exception: they
carry copyable RHIRef<RHIRenderTarget> handles for the view's render outputs, never
native pointers. Renderers record only Ready refs, and command-list copies followed
by Vulkan submission retention own the GPU-use lifetime.
