# Editor Command Contract

This contract guides implementation of the shared command boundary accepted in [ADR 02](../adr/02-agent-native-editor-control-plane.md), confirmed in [#125](https://github.com/Souvenal/SoulEngine/issues/125#issuecomment-6400427468), and documented by [#140](https://github.com/Souvenal/SoulEngine/issues/140). It specifies required behavior, not delivered runtime functionality. Read it when registering commands, wiring human input or MCP, scheduling Editor execution, or changing provider lifetime.

## Ownership And Registration

- EditorCommands owns registry metadata, generated adapters, admission validation, the shared bounded queue, phase dispatch, and transport-neutral completion. It depends on neither MCP nor HTTP.
- Editor and other command providers own business callbacks and the state those callbacks access. Launch owns startup/shutdown ordering and calls command execution on the Game/main thread. MCP depends on EditorCommands and owns protocol parsing, discovery, connection state, and HTTP/JSON-RPC responses.
- Register one unique string name, ordered named parameter descriptors, and one void business handler. Descriptors include parameter types, required/default values, bounds, description/version, execution phase/category, Agent exposure policy, and argument/result budgets. Registration validates descriptor/callback compatibility and rejects duplicate names. C++23 type deduction supplies types, not parameter names or policy.
- Generate positional C++ and named-JSON adapters from that registration. Both apply the same arity/type/default/bounds rules; JSON arguments map by descriptor name, never object iteration order. Unknown names, missing required values, and invalid types fail before admission. No per-operation command class hierarchy, EnTT meta dispatch, or heterogeneous typed-return framework is required.
- Publish immutable descriptor snapshots for discovery and thread-safe admission. Registration/retirement happens on the main thread. A provider retires its registration before destroying callback captures or state: close admission for that registration, invalidate queued invocations, settle their completions, and finish an already-running handler before releasing the provider. A re-registered name must not inherit invocations accepted for a retired registration.

The registry identity/lifetime check protects callback ownership. A command that refers to an Application, Scene, panel, or input lease also validates that target at execution time. An owning envelope does not make a borrowed Scene pointer safe across frames. Use stable identifiers where available; otherwise reject stale/unavailable targets rather than retaining borrowed state.

## Submission And Completion

SubmitByUser and SubmitByAgent share argument contracts, queueing, and the registered business handler. SubmitByUser is used by physical input and UI semantic actions; SubmitByAgent adds an owning transport-neutral completion sink. User submissions still report admission and execution failures through diagnostics. An internal command is Agent-callable only when its exposure/category policy allows it; MCP advertises that subset as bounded tools rather than offering arbitrary command or shell execution.

Admission copies or moves arguments into an owning invocation envelope containing command/registration identity, source, sequence, target phase, deadline/cancellation state where applicable, and optional completion state. Strings, JSON, and positional argument storage outlive the caller. Caller stack references and asynchronous output parameters are invalid. Direct nlohmann/json use is permitted for descriptors, named arguments, structured failures, and query output. HTTP response types stay in MCP.

The void handler receives an execution context that can record structured failure and optional JSON output. Returning normally without a recorded failure defaults to successful handler completion. The exact C++ context spelling and metadata layout belong to implementation; the observable contract is one terminal outcome per invocation, with command/invocation identity, status, frame identity when executed, and either output or a structured failure code/message/details. Failure cases include invalid arguments, unknown/unavailable command, denied exposure, queue capacity, stale target, deadline, cancellation, and shutdown.

Keep admission, handler completion, and observed effect distinct. Acceptance into the queue is not completed execution. Starting a GPU readback is not a completed pick. Commands whose advertised result requires a later effect remain pending until that defined observation, or return explicitly pending data with the latest completed frame. Completion storage owns any deferred state; disconnect/cancellation may detach transport delivery without dereferencing a dead connection. Settle terminal completion exactly once even when cancellation races dispatch or provider retirement.

MCP alone maps completion to protocol responses and request IDs. Completion delivery must be non-blocking for the main thread and must not execute network I/O or acquire transport locks while holding registry/queue locks. No main-thread handler or completion callback waits for work queued to the same thread.

## Three Execution Phases

Launch/Editor provide one ordered phase sequence per main-thread tick:

| Phase | Position And Responsibility |
| --- | --- |
| Input | After platform event collection, before ImGui frame input and EditorCameraComponent consume it. Physical events enter SubmitByUser once, and virtual input enters SubmitByAgent; shared handlers feed the existing frame-input consumers. |
| Action | At a safe main-thread point after UI action collection, outside active menu/panel callbacks. Human UI and Agent semantic actions invoke the same queued handler. Application replacement clears/rebinds the active Scene before later consumers use it. |
| Post-update | After the tick’s input, UI, action execution, and relevant Editor/Application updates have settled. Read-only queries collect one coherent observation and its Editor frame identity. |

UI callbacks submit semantic actions instead of applying the same side effect directly. The action phase may defer visible UI changes to the next UI build; panels that already drew belong to the earlier UI snapshot. Post-update reports the resulting authoritative state and identifies pending or unavailable observations rather than mixing frame-local borrowed data. This extends [ADR 11](../adr/11-editor-active-scene-system-inspection.md) while preserving its Scene ownership and clear/rebind invariant.

editor.state.get is an ordinary registered post-update query. Its baseline covers lifecycle/frame, active Application/renderer, window/framebuffer and Viewport coordinate spaces, focus/input capture, visible panels, hover, and latest available selection. Values not yet produced or unsupported are explicit unavailable/pending data; selection does not imply synchronous GPU picking.

## Bounds, Reentrancy, And Input

Each phase takes one frozen batch of eligible invocations with explicit finite queue capacity, argument/result size limits, and per-batch work limits. The implementation defines concrete budgets and validates them at the queue/dispatch seam. Preserve admission sequence among eligible invocations, subject to phase ordering and physical-user input precedence. Overflow rejects admission with a structured failure; budget exhaustion retains eligible work for a later batch without silently dropping it. Handlers are bounded; expensive asynchronous work leaves an explicit pending observation.

A frozen batch never grows during execution. Reentrant submission only enqueues: a command targeting a phase whose batch has already frozen waits until the next tick; a later phase whose batch has not frozen may accept it this tick. Dispatch does not recursively drain. Execute handlers and notify completions outside queue/registry locks. Cancellation/deadline/registration validity is checked again before starting each invocation, including one already selected into a frozen batch.

Preserve button/key down, move/held, and up transitions across the frames needed by ImGui and the Editor camera; draining one batch must not collapse a whole drag into an unobserved frame. Physical-user input revokes or interrupts the bounded Agent input lease and clears its virtual held state before conflicting Agent input executes. User events continue through the same command path if MCP is unavailable. Sequence ordering alone does not solve gesture conflicts, transactions, undo, or stale targets; those require their own explicit contracts. Internal phase batches do not imply JSON-RPC batch protocol support.

## Startup And Shutdown

Initialize EditorCommands before providers register handlers and before either input submissions or MCP admission starts. A failed MCP listener leaves the human command path available.

1. Stop new MCP requests and close SubmitByUser/SubmitByAgent admission before callback owners begin teardown.
2. Cancel unstarted work, including frozen-but-not-started invocations, and settle pending/deferred completions with structured shutdown outcomes. An already-running bounded handler finishes while its provider is alive; no worker forcibly interrupts main-thread state.
3. Release the Agent input lease and virtual held state on the main thread while input consumers remain alive.
4. Retire registrations and finish terminal notification delivery while completion storage and MCP delivery services remain alive. Stop/join protocol workers before destroying completion services or callback owners. Teardown must not wait on an undrained main-thread queue.
5. Release command services and continue normal Application/Editor, RHI, and WindowSystem shutdown under [ADR 03](../adr/03-three-thread-pipeline-variant-commands.md). Clear the borrowed active Scene binding before Application destruction.

## Consistency Review

Validate documentation against the approved issue contract and independent scenarios, then record evidence in the leaf handoff. Runtime leaves test these observable seams with the maintainer-agreed test plan; this document does not claim those tests exist.

- Reordering JSON members calls the same single handler with the same positional values; invalid/extra/missing arguments fail consistently for both entry points.
- A menu-generated action and its Agent equivalent share the handler; a Scene replacement cannot leave a queued borrowed pointer or panel binding dangling.
- Reentrant same-phase submission waits for a later tick; later-phase submission obeys the frozen boundary, and queue overflow yields one failure.
- A drag spans visible input frames and physical input interrupts the Agent lease.
- A post-update state query reports one frame and the latest completed pick, including pending/unavailable state.
- Provider retirement, disconnect, cancellation, and shutdown settle completions once while callback owners and delivery state remain alive.
