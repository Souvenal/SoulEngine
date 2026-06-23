# ADR 05 — Async Runtime Resource Readiness

## Status

Accepted (2026-06-24)

## Context

Runtime resources such as textures and graphics pipelines can require expensive
CPU work and backend-native RHI work before they are usable by a frame. Texture
preparation may include file IO, image decode, GPU upload, layout transition,
and descriptor publication. Graphics pipeline preparation may include shader
compilation, reflection, and backend pipeline creation.

The three-thread frame pipeline keeps Game, Render, and RHI work separated:
Render builds pure RHI command data, while RHI owns backend-native GPU work. A
synchronous `LoadTexture()` or `CreateGraphicsPipeline()` path conflicts with
that model because it can stall a frame thread and can also blur which thread
owns backend-native RHI state.

## Decision

SoulEngine runtime resources use asynchronous request + typed resource handles
for frame-path consumption.

Resource readiness means render-consumable readiness: the required CPU
preparation, RHI commitment, GPU transfer completion, and publication steps have
completed so the current frame can consume the resource without blocking.

The resource pipeline is split by thread ownership:

- Background TaskGraph workers perform non-thread-affine CPU work such as file
  IO, image decode, and shader compilation.
- The RHI thread performs backend-native RHI commitment, including object
  creation, descriptor writes, GPU upload submission, and completion
  publication.
- The Render thread resolves pass and draw dependencies before emitting coherent
  RHI command data.

Renderers hold typed resource handles, not final RHI payloads directly. A
resource handle refers to the resource identity, state, generation, and eventual
payload. The ready payload may be an `RHI::Texture`, `RHI::GraphicsPipeline`, or
other RHI object, but the payload is not the resource identity.

Equivalent in-flight requests are coalesced by canonical resource key and return
handles to the same resource. Asynchronous completions carry a resource
generation and may publish only when their generation still matches the current
resource generation.

When a resource is not ready on the runtime frame path, the default policy is to
skip the dependent work. Consumers may opt into fallback resources when a
coherent substitute exists. Blocking waits are reserved for startup, tooling,
tests, or other non-frame-path operations.

Async Resource v1 covers texture resources and graphics pipeline resources.
Buffer resources may follow the same model later. Constant buffers, per-frame
uniform buffers, swapchain images, and transient render targets are outside this
decision.

Priority scheduling and explicit cancellation are deferred. The initial model
does not require killing running CPU, RHI, or GPU work.

## Consequences

This preserves the non-blocking frame path and keeps backend-native RHI mutation
owned by the RHI thread. It also gives render code a precise way to distinguish
loading, ready, failed, fallback, and stale completion cases instead of encoding
all of them as `nullptr` or a boolean.

The trade-off is a more complex resource layer. Synchronous resource creation
APIs become transitional paths or non-frame-path utilities, and renderer code
must resolve dependency sets before emitting command lists. This extra layer is
intentional because it prevents long-running preparation work from becoming
hidden frame hitches.
