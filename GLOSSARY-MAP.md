# Glossary Map

## Contexts

- [Core](./Engine/Source/Runtime/Core/GLOSSARY.md): foundation types, logging, config (CVars), and utilities shared by every module
- [TaskGraph](./Engine/Source/Runtime/TaskGraph/GLOSSARY.md): cross-thread task dispatch — background workers and named thread queues
- [WindowSystem](./Engine/Source/Runtime/WindowSystem/GLOSSARY.md): window-system abstraction; GLFW behind one interface
- [RHI](./Engine/Source/Runtime/RHI/GLOSSARY.md): abstract GPU layer — resources, views, frame packets, submission and destruction contracts
- [Vulkan](./Engine/Source/Runtime/RHI/Vulkan/GLOSSARY.md): the Vulkan RHI backend — timelines, rings, belts, deferred destruction
- [Shader](./Engine/Source/Runtime/Shader/GLOSSARY.md): shader data model — the artifact contract between compiler and RHI
- [ShaderCompiler](./Engine/Source/Runtime/ShaderCompiler/GLOSSARY.md): Slang-based offline shader compilation
- [Material](./Engine/Source/Runtime/Material/GLOSSARY.md): renderer-neutral material records and texture semantics
- [Scene](./Engine/Source/Runtime/Scene/GLOSSARY.md): mutable world state, scene documents, and per-frame snapshots
- [Renderer](./Engine/Source/Runtime/Renderer/GLOSSARY.md): raster and ray-tracing renderers — passes, view targets, geometry upload
- [RenderGraph](./Engine/Source/Runtime/RenderGraph/GLOSSARY.md): per-frame graph — declarations, ordering, pruning, pooling
- [Application](./Engine/Source/Runtime/Application/GLOSSARY.md): project-level application identity and scene ownership
- [Launch](./Engine/Source/Runtime/Launch/GLOSSARY.md): engine bootstrap and the three-thread frame pipeline
- [Editor](./Engine/Source/Runtime/Editor/GLOSSARY.md): Dear ImGui editor — panels, editor world, draw-data snapshots

## Relationships

- **Core → everything**: every module speaks Core's type aliases, ErrorMessage, CVars, and logging.
- **TaskGraph → Launch / RHI**: provides the named thread queues (Game/Render/RHI) the three-loop pipeline and frame-affined tasks ride on.
- **WindowSystem → RHI**: supplies the native window handle the backend builds its presentation surface on.
- **Application → Scene**: an Application owns the mutable Scene; opening an application loads its Default Scene Document.
- **Scene → Renderer**: the game thread snapshots Scene into an immutable SceneSnapshot; renderers consume snapshots, never the mutable Scene.
- **Material → Scene / Renderer**: Material owns renderer-neutral records; Scene caches imported materials, renderers resolve them per draw.
- **Shader ↔ ShaderCompiler / RHI**: Shader is the data contract — ShaderCompiler produces GraphicsProgram artifacts, RHI consumes them, and neither depends on the other.
- **Renderer → RenderGraph → RHI**: renderers build passes with declared views on the graph; Compile turns declarations into RHI pass entries with prologue transitions (ADR 05).
- **RHI → Vulkan**: the backend self-registers through the RHI factory; RHI never imports a concrete backend.
- **Launch → all of the above**: EngineLoop owns startup/shutdown order and drives the Game → Render → RHI frame pipeline (ADR 03).
- **Editor → Scene / RHI**: editor panels inspect the active scene through the ADR 11 bindings; the editor renders through the RHI presentation overlay.
