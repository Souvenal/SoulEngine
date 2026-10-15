# Editor

Editor UI subsystem. Dear ImGui is the engine's only UI system — there is deliberately no UI abstraction layer. The ImGui context lives on the main thread; the render thread is a pure consumer of draw-data snapshots.

## Language

**Editor**:
Engine-loop-owned subsystem: the ImGui context, the editor world, the platform backend, the panel registry, and the GPU resources for rendering ImGui draw data through the RHI command list.

**EditorWorld**:
Editor-owned ECS world containing editor-only entities, systems, and the viewport camera — isolated from the runtime Scene registry.

**UIDrawFrame**:
Self-owning deep copy of one frame of ImGui draw data, handed from the main thread to the render thread through a latest-wins mailbox.

**UIPanel**:
One registered debug/editor panel: a name plus an immediate-mode callback invoked in registration order during the frame build.

**EditorCameraComponent**:
Editor-only camera component: camera parameters, the editor Viewport, picking readback state, and selected-entity state. Holds no RHI resources; the editor view's render targets are renderer-created pooled graph transients (ADR 05).

**Selected render pixel**:
The latest editor scene-view pixel chosen by the user, copied into the immutable snapshot and consumed by renderer post-processing to identify the selected entity id.

**Active Scene binding**:
A non-owning scene pointer held by the Editor for the current UI tick — bound after Application creation, cleared before Application destruction, never retained across frames (ADR 11).

**Live system query**:
A read-only Scene system lookup performed by a panel at draw time, with the returned inspection data consumed immediately (ADR 11).
