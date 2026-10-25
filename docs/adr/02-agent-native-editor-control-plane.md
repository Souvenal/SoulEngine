# ADR 02 — Agent-native Editor Control Plane

**Status:** accepted

SoulEngine will embed a loopback-only Model Context Protocol (MCP) host in the Engine process so an MCP-compatible agent can observe and operate the Editor. The MCP host is a replaceable protocol adapter, not an Editor domain model: the Editor main thread remains the authority for the ImGui context, EditorWorld, EditorCameraComponent, CVars, and all future mutable editor state. This shape gives an agent native access to Engine capabilities without requiring a sidecar process or duplicating the existing WindowSystem to ImGui to Editor input path.

## Context

The Editor currently accepts human WindowSystem input, renders immediate-mode ImGui UI, and exposes a small set of runtime configuration and Viewport capabilities. A capable agent needs the same ability to observe Editor state and to operate CVar, Viewport, menu, panel, Scene, and later persistence workflows. Direct access from a network thread would violate the Editor's main-thread ownership and would make UI, input, selection, and Scene effects non-deterministic.

MCP is the chosen agent-facing protocol because clients can discover bounded tools and their input schemas. It does not define the Engine's editing model. The production Game task queue is currently not drained, so it cannot serve as the dispatch mechanism for Editor operations.

## Decision

### In-process MCP host

The Engine owns an embedded MCP Streamable HTTP host. It binds only to loopback, defaults to an OS-assigned port, and publishes the resolved endpoint in an instance-specific local discovery record. A developer may configure a fixed loopback port for local development. The host does not scan ports or bind to a non-loopback interface.

Foundation supports one explicitly pinned stable MCP protocol release. It implements only the required initialization, tool discovery, tool call, and JSON-RPC error behavior for the tools it advertises. Resources, prompts, sampling, elicitation, OAuth, remote sessions, and broad feature parity are not Foundation commitments. A protocol upgrade is an explicit compatibility decision, not an implicit move to a draft or latest revision.

There is no official C++ MCP SDK. Foundation therefore owns a deliberately small MCP adapter rather than initially adopting a community C++ SDK. The adapter is replaceable: a future library adoption requires a pinned revision, license and dependency review, Clang and xmake validation, MCP Inspector and supported-client interoperability, and applicable official conformance validation.

The MCP adapter may use version-pinned C++ HTTP and JSON building blocks that meet the repository dependency policy. Those libraries run on protocol worker threads and remain hidden behind the adapter boundary; Core, WindowSystem, Editor, Scene, and CVar APIs do not expose HTTP or JSON types.

### Main-thread command boundary

The MCP host validates requests and converts them into immutable Agent Commands. A dedicated Agent Command Queue passes those commands to the Editor main-thread tick, which serially executes the corresponding handler and completes the response. Protocol workers never access thread-affine Editor, ImGui, EditorWorld, or mutable Scene state.

Each registered tool has an immutable descriptor: name, human and agent readable description, tool version, input schema, execution category, request and result budgets, and main-thread handler. Tool discovery derives from the registry. Schema, size, and method validation happen before an Agent Command enters the queue, so Editor code never receives arbitrary transport JSON.

Each call has a deadline. Cancellation, client disconnect, deadline expiry, and Editor shutdown discard a command that has not started. A handler already running on the Editor main thread reaches its defined atomic completion point; protocol workers do not forcibly interrupt ImGui, CVar, input, or Editor work. Every result identifies the request and Editor frame and reports whether work is pending, completed, cancelled, or failed. Enqueued work and started asynchronous readback are not reported as verified effects.

### Foundation tools and input ownership

Foundation exposes bounded observation and input tools only. Its initial product scope is baseline Editor state, CVar observation and runtime switching, and frame-scoped Viewport and ImGui input. CVar changes affect in-memory runtime state; project-setting persistence is a later capability.

Agent input merges at the existing WindowSystem per-frame input seam and is consumed by the current ImGui and EditorCameraComponent paths. A bounded control lease serializes agent input. Physical user input remains authoritative and interrupts or revokes the agent lease. Current Viewport picking is asynchronous; tools report the latest completed observation and frame identity rather than promising synchronous object selection.

Tool descriptors classify execution as `observe`, `input`, `edit`, `persist`, or `destructive`. Foundation enables only `observe` and lease-bound `input`. Later tool registration does not grant authority by default; session capability policy and remote authorization are later work.

### Security and lifecycle

Loopback is not a trust boundary. Foundation validates Origin, bounds request body size, JSON depth, tool arguments, concurrent connections, queued commands, and deadlines, and rejects unsupported methods and unknown tools.

Editor shutdown first stops new MCP requests, discards unstarted Agent Commands, releases agent input lease and virtual input state, and stops protocol workers before Editor, WindowSystem, or RHI teardown. MCP host startup failure disables Agent Control but does not prevent normal Editor startup.

## Considered Options

### External MCP sidecar

A sidecar can reuse an official TypeScript or Python SDK and isolate protocol runtime failures. It adds a process, startup coordination, local IPC, and an additional deployment boundary to the normal Editor workflow. An embedded host keeps the Editor lifecycle and local MCP endpoint together while retaining the same Engine-side Agent Command Queue.

### Community C++ MCP SDK

Community C++ libraries can reduce protocol implementation work and may later be adopted after independent validation. No such library has official C++ SDK governance, and using one immediately would couple Foundation to an unverified protocol, transport, and dependency surface larger than the required tools subset.

### Embedded non-C++ runtime

Embedding a Node, Python, Go, or Rust runtime would enable an official SDK but increases binary, packaging, ABI, threading, security, and lifecycle complexity. SoulEngine does not already ship one of these runtimes for another purpose.

## Consequences

Foundation gains a direct local MCP integration with no sidecar startup step and preserves a strict boundary between protocol concerns and Editor behavior. It also assumes ownership of a narrow but evolving protocol adapter and its interoperability tests. Subsequent roadmap capabilities can add rich observation, semantic Scene operations, transactions, persistence, semantic UI inspection, and remote policy without changing the fundamental command or thread-ownership model.
