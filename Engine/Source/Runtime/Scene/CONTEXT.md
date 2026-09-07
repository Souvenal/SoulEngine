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
| **Scene File** | One YAML file containing exactly one Scene Document with an `entities` root list for the Document Tree. The initial format omits an explicit schema-version field. |
| **Scene Entity** | A runtime entity represented in the Scene registry. Every Scene Entity has exactly one Scene Node and is therefore spatial. |
| **Scene Node** | The mandatory structural record for a Scene Entity. It owns the derived world matrix, parent-child relationship, and child order; local transform authoring is represented by the optional `components.transform` component. |
| **Scene Registry** | The ECS registry owned by a Scene. It contains Scene Entities only; non-spatial runtime concerns do not belong in it. |
| **Authoring State** | Scene data intentionally represented in a Scene Document and editable by people or AI agents. |
| **Component Map** | The `components` mapping on an entity node. Each key names one component kind and each value is that component's authoring data; a Scene Entity has at most one component of each kind. Components are optional in the document and receive runtime defaults when omitted. |
| **Component Type Naming** | Optional ECS component types use the C++ suffix `Component`, such as `CameraComponent`, `LightComponent`, and `MeshComponent`. Their Scene Document names are defined independently by EnTT meta type registration. |
| **MeshComponent** | Optional component that associates a Scene Entity with mesh content to be rendered. Its Authoring State is an asset path and optional material YAML override path, both relative to the current application Assets directory. Renderer-specific mesh resource refs, uploads, and GPU representations are renderer-owned rather than component state. |
| **Structural Error** | A Scene Document error that prevents a valid Document Tree or mandatory Scene Node data from being constructed. It rejects the whole Scene File. |
| **Component Warning** | A recoverable issue in one optional component, including an unknown component, unknown field, or invalid component value. The loader warns and omits that component while loading the remainder of the Scene. |
| **Scene Loading** | Scene-owned, non-public YAML input implementation that constructs a Runtime World without exposing parser-specific types in the Scene API. |
| **Scene Replacement** | V1 loading constructs a complete temporary Runtime World and atomically replaces the current Scene only after all structural data is valid. It does not merge or patch an existing Scene. |
| **Runtime State** | Ephemeral state created while a Scene runs. It is not represented in a Scene Document. Component-private Runtime State may live beside that component's Authoring State; only shared or renderer-owned state must live elsewhere. |
| **World Coordinate System** | The Scene uses a right-handed, Y-up coordinate system. Asset-format coordinate differences are converted at an asset-import boundary. |
| **Transform** | `Core:ECS.Transform` component containing local translation, rotation, and scale data. Scene Documents express it as `components.transform`, with rotation as Euler angles in degrees applied in X → Y → Z order about fixed parent-space axes; the runtime adds a default TransformComponent when omitted and derives its world matrix through the Scene Hierarchy. |
| **SceneSnapshot** | Immutable per-frame render view built from `Scene` at the end of the GameLoop and held by the frame slot. It contains camera views, value-semantic geometry/material `InstanceRecord` values, and optional editor selection input. |
| **PixelCoordinate** | A physical framebuffer pixel coordinate carried as optional editor selection input. The renderer post-process reads the EntityId G-buffer at this coordinate to determine the selected ID. |
| **GBuffer** | Camera-owned ref-backed render-target set containing albedo, normal, material ID, entity ID, and one shared depth target. The depth target is both the geometry-pass depth attachment and the deferred lighting sampled depth resource. |
| **CameraRenderTargets** | Camera-owned output bundle containing the GBuffer and the final SceneColorRT render target. Post-process passes load SceneColorRT so they can overlay results without replacing the lighting image. |
| **CameraViewRecord** | One immutable camera/view record defined with the Camera component family: view-projection data plus ref-backed CameraRenderTargets. Renderers allocate their own transient constant buffers while recording the frame. |
| **GeometryRecord** | Scene-owned imported geometry record containing CPU vertex metadata, local bounding sphere, ref-backed position, normal, tangent, UV, and index buffers, and a `GpuData` shader-address ABI constructed by `BuildGpuData()`. It has no material identity. Bitangents are not stored; consumers derive them from normal and tangent. |
| **SubMesh** | One imported `aiMesh` name, geometry handle, and default imported `MaterialHandle`. It preserves the material binding at imported-geometry scope rather than in GeometryRecord. |
| **MeshAssetNode** | One static node from an imported asset hierarchy. It stores its local transform, ordered child nodes, and ordered references to `SubMesh` values; multiple nodes may reference the same SubMesh. |
| **MeshRecord** | Scene-local cached imported asset. It owns a normalized absolute asset path, the complete imported material table, deduplicated SubMesh values, and an immutable MeshAssetNode tree shared by every entity that instances that mesh. |
| **Material inspection query** | A transient flat `std::vector<ConstMaterialHandle>` returned by MeshSystem. The Editor groups and displays records by their MaterialRecord provenance during the current draw. |
| **InstanceRecord** | Value-semantic SceneSnapshot record for one geometry/material instance. It carries the entity ID, GeometryRecord handle, selected MaterialHandle, and derived world transform. |
| **CameraComponent** | Optional component describing a camera attached to a Scene Entity. It persists only authoring camera data and may retain component-private runtime view state; control behaviour is separate Runtime State. |
| **LightComponent** | Optional authoring component describing a Directional or Point light attached to a Scene Entity. Spot lights and shadow flags are not part of the current contract. |
| **LightRecord** | Immutable world-space light record in a GameSnapshot. It carries the source entity ID, physical light values, and the shared 48-byte `GpuData` ABI consumed by Raster and RayTracing renderers. |
| **Scene system query** | A templated lookup of one registered Scene system. It returns an optional borrowed reference wrapper, preserving constness and hiding the Scene's `SystemScheduler` from consumers. |

## Architecture

`Scene` is the mutable Runtime World owned by Application. Its Scene Registry
contains Scene Entities only, each with one mandatory Scene Node. Scene Nodes
form the ordered Document Tree hierarchy and derive world transforms from local
transforms.

`SceneSnapshot` is the immutable render-facing view copied into the frame slot
at the end of the GameLoop. The renderer consumes `SceneSnapshot` each frame
via `IRenderer::Render()`. MeshSystem owns Scene-local MeshRecord,
GeometryRecord, and MaterialRecord caches. MeshSystem expands each
MeshComponent into one InstanceRecord per asset-node SubMesh reference, pairing
shared GeometryRecord and MaterialHandle values with the accumulated asset-node
transform followed by the entity-derived world transform. Scene-authored
material-name override resolution remains a later migration step. Each renderer
resolves the material handle into its own draw-instance representation.
When a Scene record is uploadable, it owns the explicit GPU ABI mirror and
builder beside its RHI resource fields. The renderer selects and deduplicates
records for a frame, but does not redefine their address layout.
Material instances resolve through MeshSystem's Scene-local caches: mesh import
publishes per-slot asset materials, while a valid mesh material YAML override
replaces the material for every submesh and falls back to the imported material
when loading fails. MeshSystem's Editor inspection query rebuilds its material
handle list on demand; callers retain no result handles across frames.

`Scene` owns shared Scene model types. Each component family owns one
`Scene:<Name>` partition and its internal static EnTT meta registration.
`Scene:YamlIO` resolves component and field names through this metadata, then
directly emplaces the built-in component types.

Scene Loading is an internal Scene implementation. It uses a single-document
YAML subset of mappings, sequences, and scalars without exposing parser-specific
types through the Scene API. Anchors, aliases, explicit tags, directives, and
duplicate mapping keys are Structural Errors. V1 loads a complete replacement
Runtime World, reports Structural Errors for invalid required tree data, and
reports Component Warnings while retaining runtime defaults for invalid optional
components.

Authoring State and component-private Runtime State may coexist in one
component type. Each component's explicitly registered EnTT meta data fields
participate in Scene Document loading. Renderer-owned resource caches retain asset-resource ownership while resolving the
asset identities in snapshots. CameraViewRecord additionally carries the camera-owned
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
