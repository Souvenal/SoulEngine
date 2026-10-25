# ADR Catalog

ADRs record hard-to-reverse project and architecture decisions: system shape, cross-module boundaries, technology lock-in, non-obvious constraints, and rejected alternatives that a future maintainer would otherwise reopen. Coding rules and detailed implementation guidance belong in the standards hierarchy.

## Before editing an ADR

Read this catalog and the ADRs in the affected area before creating, renumbering, superseding, or replacing an ADR. Existing numbering is authoritative: use the next available two-digit number, preserve historical gaps, and do not renumber accepted ADRs for cosmetic sequence.

## Writing

Write ADR prose as ordinary Markdown paragraphs: one logical paragraph per physical line. Do not hard-wrap prose to a comment-width limit. Headings, lists, tables, blockquotes, and fenced code blocks keep their structural line breaks.

Start each ADR with # ADR NN — Title, where NN matches its filename NN-slug.md. Use **Status:** accepted, proposed, deprecated, or a supersession note when status matters. Add Context, Decision, Considered Options, and Consequences only when they clarify the decision; an ADR should retain the decision and rationale rather than procedural implementation detail.

## Current numbering

The catalog currently contains ADR 01, 02, 03, 04, 05, 06, 08, and 11. The gaps are historical and remain valid.
