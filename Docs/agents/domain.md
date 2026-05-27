# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`CONTEXT.md`** at repo root — global project context + resolved terms.
- **`CONTEXT-MAP.md`** at repo root — index pointing to one `CONTEXT.md` per module. Read each one relevant to the topic.
- **`Docs/ADR/`** — read ADRs that touch the area you're about to work in. Also check module-local `Docs/ADR/` directories for context-scoped decisions.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The producer skill (`/grill-with-docs`) creates them lazily when terms or decisions actually get resolved.

## File structure

Multi-context — each module has its own `CONTEXT.md`:

```
/
├── CONTEXT.md                        <- global context + resolved terms
├── CONTEXT-MAP.md                    <- map to per-module contexts
├── Docs/ADR/                         <- cross-module ADRs
└── Engine/Source/Runtime/
    ├── Core/CONTEXT.md
    ├── Window/CONTEXT.md
    ├── Application/CONTEXT.md
    ├── Launch/CONTEXT.md
    ├── RHI/CONTEXT.md
    ├── Shader/CONTEXT.md
    └── ShaderCompiler/CONTEXT.md
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `CONTEXT.md`. Don't drift to synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal — either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/grill-with-docs`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (event-sourced orders) — but worth reopening because…_
