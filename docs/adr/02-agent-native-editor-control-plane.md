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

The MCP adapter may use version-pinned C++ HTTP and JSON building blocks that meet the repository dependency policy. Direct nlohmann/json use is permitted in MCP, EditorCommands, and command-facing Editor contracts, including named arguments, schemas, structured failures, and optional query output. HTTP and JSON-RPC response construction remain MCP responsibilities; using JSON does not grant a worker access to Editor state. Dependency versions and the stable MCP protocol release still require build and interoperability validation.

### Shared main-thread command boundary

EditorCommands and MCP are separate modules. EditorCommands owns the shared registry, argument validation, owning invocation envelopes, bounded queue, phase scheduling, and transport-neutral completion. Editor and other command providers own business handlers and thread-affine state. MCP owns protocol discovery, request validation, and HTTP/JSON-RPC responses. Human input and UI actions enter the same registry and queue as agent operations; the Editor main-thread tick is the sole execution authority. Protocol workers never access thread-affine Editor, ImGui, EditorWorld, or mutable Scene state.

SubmitByUser and SubmitByAgent share argument contracts and one registered void business handler per string name. Registration provides ordered named parameter descriptors and generates positional C++ and named-JSON adapters; JSON member order has no meaning. The execution context supports structured failure and optional JSON output. The Agent entry attaches a transport-neutral completion sink; the User entry retains validation and failure diagnostics without requiring that sink. EnTT meta, per-operation command class hierarchies, and heterogeneous C++ return frameworks are outside this design.

Descriptors carry descriptions, versions, named parameter types/defaults/bounds, schema, execution phase/category, exposure policy, and request/result budgets. MCP tool discovery derives from the explicitly exposed subset; registering an internal human command does not expose it to agents. Both entry points validate arguments before queue admission, and execution revalidates live targets and policy.

The [Editor command contract](../standards/editor-commands.md) defines registration lifetime, owning arguments/completions, and the input, action, and post-update phases. Each phase executes a frozen bounded batch in deterministic order; reentrant submission enters a later eligible batch. Input transitions retain frame visibility and physical-user precedence. This scheduling does not confer transactions, gesture conflict resolution, or undo.

Each Agent call has a deadline. Cancellation, client disconnect, deadline expiry, provider retirement, and Editor shutdown discard unstarted work with a structured terminal outcome. A handler already running on the Editor main thread reaches its bounded completion point while its provider remains alive. Results identify invocation and Editor frame and distinguish admission, handler completion, and observed frame effect. A void handler returning normally defaults to successful handler completion; queued work or a started asynchronous readback does not prove the downstream effect. The main thread never waits for its own queued work.

### Foundation tools and input ownership

Foundation exposes bounded observation and input tools only. Its initial product scope is baseline Editor state, CVar observation and runtime switching, and frame-scoped Viewport and ImGui input. CVar changes affect in-memory runtime state; project-setting persistence is a later capability.

Human platform input enters SubmitByUser once; agent input enters SubmitByAgent. The shared input phase feeds the existing WindowSystem frame-input seam and the current ImGui and EditorCameraComponent consumers. UI-generated semantic actions enqueue the same handler as the corresponding agent action. A bounded control lease serializes agent input. Physical user input remains authoritative and interrupts or revokes the agent lease. Current Viewport picking is asynchronous; tools report the latest completed observation and frame identity rather than promising synchronous object selection.

Tool descriptors classify execution as `observe`, `input`, `edit`, `persist`, or `destructive`. Foundation enables only `observe` and lease-bound `input`. Later tool registration does not grant authority by default; session capability policy and remote authorization are later work.

### Security and lifecycle

Loopback is not a trust boundary. Foundation validates Origin, bounds request body size, JSON depth, tool arguments, concurrent connections, queued commands, and deadlines, and rejects unsupported methods and unknown tools.

Editor shutdown closes MCP admission and both shared submission entry points, settles unstarted invocations, lets a running bounded handler finish, releases agent input lease and virtual input state, retires registrations, and stops protocol workers before callback owners, Editor, WindowSystem, or RHI teardown. Completion delivery remains alive until terminal notifications are settled. MCP host startup failure disables Agent Control but leaves the shared human command path available.

## Considered Options

### External MCP sidecar

A sidecar can reuse an official TypeScript or Python SDK and isolate protocol runtime failures. It adds a process, startup coordination, local IPC, and an additional deployment boundary to the normal Editor workflow. An embedded host keeps the Editor lifecycle and local MCP endpoint together while retaining the same Engine-side shared command boundary.

### Community C++ MCP SDK

Community C++ libraries can reduce protocol implementation work and may later be adopted after independent validation. No such library has official C++ SDK governance, and using one immediately would couple Foundation to an unverified protocol, transport, and dependency surface larger than the required tools subset.

### Embedded non-C++ runtime

Embedding a Node, Python, Go, or Rust runtime would enable an official SDK but increases binary, packaging, ABI, threading, security, and lifecycle complexity. SoulEngine does not already ship one of these runtimes for another purpose.

## Consequences

Foundation gains a direct local MCP integration with no sidecar startup step and preserves a strict boundary between protocol concerns and Editor behavior. It also assumes ownership of a narrow but evolving protocol adapter and its interoperability tests. Subsequent roadmap capabilities can add rich observation, semantic Scene operations, transactions, persistence, semantic UI inspection, and remote policy without changing the fundamental command or thread-ownership model.
