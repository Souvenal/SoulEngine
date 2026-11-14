# Agent Workflow

Agent-only behavior: recovering issue context, handing work across session boundaries, and delegating to subagents. The shared development workflow (issue-first discussion, worktrees, review gates, integration) lives in [dev-workflow.md](../development/dev-workflow.md); this document owns what only agents do.

## Issue Context Recovery

Before planning, implementing, reviewing, or resuming a ticket:

1. Resolve the current issue and its full parent chain up to the root spec issue. Read every issue body and all comments, including every API page, in root-to-leaf order. A milestone groups work; its description does not replace the root spec or intermediate capability issues. Use the [tracker retrieval commands](../agents/issue-tracker.md#recover-complete-issue-context).
2. Read the referenced ADRs and relevant PR discussion or review decisions. Check direct blockers, current PR/merge state, branch tips, and repository state. Follow links that bear on this ticket; sibling discussion is loaded only when referenced or required by a dependency.
3. Separate confirmed decisions, proposals, observed facts, and open questions. Use the newest handoff as a navigation aid, while retaining later discussion and explicit supersession links. Chronological recency alone does not authorize an architectural or scope change. Report unresolved contradictions between the issue contract, comments, ADRs, and repository workflow before dependent implementation.
4. Before editing, give a concise recovery summary identifying the current scope and acceptance, inherited constraints, unresolved questions, actual implementation base and PR target, and next action. Recovery is complete only when the ancestor chain and all comment pages were read and these items are accounted for. If retrieval fails or is truncated, report exactly what is missing and continue only with work independent of that gap.

## Session Handoff

### Minimal invocation

`/handoff` or “收尾本次 session” invokes this entire procedure. Infer the active ticket, capability, and root from the conversation, branch/PR association, and tracker parent links; an explicit issue number selects the focus. For a session spanning multiple tickets, partition the useful context by scope. Ask only when the destination remains ambiguous after inspection. The user need not repeat destinations, formatting, evidence, or publication instructions. An explicit “local only” request keeps the handoff unpublished.

Read existing handoffs and later comments before publishing. Reuse unchanged handoffs by returning their URLs; add a new comment only for materially new context or a superseding continuation state. A local handoff file required by a skill is a preparation artifact; the default repository handoff also publishes the useful Markdown to the appropriate issues and verifies it by reading back the comments. Return those durable links and one short next-session prompt naming the next issue and action. A handoff request alone does not authorize commits, PR creation, merge, issue closure, or cleanup.

For a fresh session, “继续 #<ticket>” invokes issue-context recovery. Recover any clearly unfinished task from the latest valid handoff; when the requested action is unclear, propose the next action after recovery instead of treating continuation as automatic permission to publish a PR or merge.

### Publication contract

Publish a compact handoff when design decisions converge, a session ends or transfers work, implementation pauses, or a review handoff is ready. When the user requests ticket work or a session handoff, publishing the scoped context comment is part of that request; approval gates for PR readiness, merges, issue closure, and cleanup remain separate.

Place each summary on the narrowest issue whose scope it affects: program-wide design on the root spec, capability interfaces or integration context on the capability issue, and implementation progress on the leaf ticket. Link broader context instead of copying it into every child. Post upward only when a finding changes an ancestor's scope, contract, shared constraints, or integration status.

Issue bodies own current scope and acceptance; ADRs own accepted architecture; PRs and commits own delivered changes and validation evidence. Comments carry otherwise-lost rationale, open decisions, and continuation state. Update the owning artifact when its confirmed contract changes, and link it from the comment. Mark proposals and unverified research explicitly.

Use this comment structure, omitting empty sections:

~~~markdown
## Session handoff — YYYY-MM-DD

### Read first
Links to authoritative artifacts and any broader handoff.

### Carry forward
Otherwise-lost rationale and constraints relevant to this issue.

### Open decisions
Pending choices, conflicts, and evidence still needed.

### Current state
Completed and remaining work; branch, worktree, commits, PR, and validation when applicable.

### Next action
The first concrete continuation step and useful skills.
~~~

A handoff identifies the issue, snapshot date, scope, and any earlier handoff it supersedes. Link superseded decisions explicitly and retain useful historical discussion; routine tool output stays in the session. A local temporary handoff file may be useful during preparation, but publish useful Markdown inline so another machine can recover it.

Before ending, read back published comments, provide their URLs, and distinguish delivered work from proposals and remaining work. A comment or local file alone does not establish a commit, passing validation, approval, or merge.

## Subagent Role Registry

Roles split by write permission and context boundary, not by domain. A lead session (the agent the user talks to) owns scope, dispatch, and synthesis; it is not itself a registry role. All registry roles are one-shot dispatches, not standing processes; dispatch a fresh subagent per task.

| Role | Permission | Model tier | Owns |
| --- | --- | --- | --- |
| `explorer` | read-only | cheap/fast | Codebase survey, call-site tracing, impact maps, hazard lists; high-volume reading with a low hit rate, compressed into evidence-backed findings |
| `implementer` | workspace-write | strong | One owned slice in a dedicated worktree; full implementation judgment, including the inner edit-build-test loop |
| `reviewer` | read-only | strong | Independent diff review against CODING_STANDARDS.md and the originating issue; low-frequency, high-leverage |
| pre-submit verification | read-only (one-shot subagent) | medium | Clean-context full build plus test suite; diagnoses to root cause, fixes return to the implementer |

Rules:

- **No standing build-validator role.** The implementer fixes build errors it introduced; verification is a step, and CI is the final gate.
- **Separate explorer and reviewer from implementation.** The implementer's context carries sunk cost and author's bias; exploration and review read without it.
- **Explorer output is a map, not file contents.** Every finding carries `path:line` evidence plus a short quote; unsupported inference is marked as speculation. The implementer reads the marked locations, not the explorer's whole reading history.
- **The implementer keeps full design judgment.** The explorer answers *where* and *what* (terrain, hazards, impact); the implementer owns *how*. Implementation judgment unfolds while writing code; it is not pre-computed.
- **Publish role output as durable artifacts.** An explorer's map, a review handoff, and a session handoff are issue comments or worktree documents following the handoff structure above; another session, agent, or machine recovers from them without this conversation.

## Dispatch Contract

Every subagent dispatch states four things; a dispatch missing any of them is not ready:

1. **Goal** — the concrete outcome, referenced to the issue and agreed scope.
2. **Boundaries** — files, paths, or decisions the role owns; everything else is reported back, not acted on. Read-only roles receive no write surface.
3. **Output format** — the artifact shape (findings list, diff, two-tier review findings, build report) and where it lands (issue comment, commit, report to lead).
4. **Completion criteria** — an observable condition that distinguishes done from not-done (for example: "every file in the change set appears in the report").

Dispatch is a compressed handoff: apply the session-handoff structure (Read first / Carry forward / Open decisions / Current state / Next action) to the single responsibility.

## Collaboration Discipline

- Subagents coordinate through durable repository artifacts, not message streams. A follow-up exploration is a new one-shot dispatch with a narrowed question, not a chat thread; messages carry exceptions only (a blocker that changes scope).
- Subagents return compressed conclusions; intermediate reading, failed attempts, and exploration debris stay in the subagent context.
- Start from the default: do the work inline. Dispatch a subagent only when isolation pays — a large exploration surface, an independent second work stream, or a review that must not share the implementer's context.
