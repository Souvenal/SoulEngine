# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`GLOSSARY-MAP.md`** at the repo root: it points at one `GLOSSARY.md` per module context. Read each one relevant to the topic.
- **`docs/adr/`**: read ADRs that touch the area you're about to work in.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs`) creates them lazily when terms or decisions actually get resolved.

## File structure

Multi-context — one `GLOSSARY.md` per module context, indexed by the root map:

```
/
├── GLOSSARY-MAP.md                     <- map to per-module glossaries
├── docs/adr/                           <- cross-module ADRs
└── Engine/Source/Runtime/
    ├── Core/GLOSSARY.md
    ├── WindowSystem/GLOSSARY.md
    ├── Application/GLOSSARY.md
    ├── Editor/GLOSSARY.md
    ├── Launch/GLOSSARY.md
    ├── RHI/GLOSSARY.md
    ├── RHI/Vulkan/GLOSSARY.md
    ├── Shader/GLOSSARY.md
    ├── ShaderCompiler/GLOSSARY.md
    ├── Material/GLOSSARY.md
    ├── Renderer/GLOSSARY.md
    ├── RenderGraph/GLOSSARY.md
    ├── Scene/GLOSSARY.md
    └── TaskGraph/GLOSSARY.md
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `GLOSSARY.md`. Don't drift to synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal — either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR 05 (declaration-driven sync) — but worth reopening because…_
