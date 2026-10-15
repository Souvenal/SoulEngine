---
paths:
  - "Engine/Source/**/*.h"
  - "Engine/Source/**/*.hpp"
  - "Engine/Source/**/*.cpp"
  - "Engine/Source/**/*.cppm"
  - "Engine/Shaders/**/*.slang"
  - "Applications/*/Shaders/**/*.slang"
---

# Comment Specifications

Rules for where comments live and what they document. Language-specific behavior and code-form rules stay in their own specs; this file owns only comment placement and content.

## Placement

- Use Doxygen-style comments (`///`) for public API documentation.
- A comment documents the declaration it precedes — never its siblings. Type-level comments describe the type as a whole (what the axis is, where the vocabulary comes from); per-member semantics belong on each member. This applies to enum enumerators and to struct/class data members alike.

  ```cpp
  // Bad: per-value semantics piled into the enum header comment.
  /// Placement decides the upload mechanism: GpuOnly is a transfer
  /// destination; CpuToGpu is host-visible.
  enum class RHIMemoryUsage : Uint8 { Unknown, GpuOnly, CpuToGpu };

  // Good: header says what the enum is; each value says what it means.
  /// Memory placement of a buffer allocation; decides the upload mechanism.
  enum class RHIMemoryUsage : Uint8 {
      Unknown,
      /// Device-local transfer destination; upload rides a staging copy.
      GpuOnly,
      /// Host-visible; upload is an RHI-thread memory copy.
      CpuToGpu,
  };
  ```

## Form and Width

- Comment lines wrap at ~110 characters; the 80-column habit is banned.
- Exactly two documentation forms exist. Short: a single `/// @brief ...` line. Long: full Doxygen structure — `@brief` summary, a blank `///` separator, detail paragraphs, `@param`/`@return` as needed. The intermediate form (multi-line prose without `@brief`) is banned.
- Short member notes use trailing `///< ...`; multi-line explanations go above the member as `///` blocks.

## Type-Level Content

- A type's comment states its purpose, its usage flow (e.g. create → record → poll), and its thread constraints. Internal mechanisms belong on the member-function comments that implement them, never on the type.
- A type's comment never names its downstream consumers; usage instructions belong to the consumer's own comments.

## Content

- Comments describe the code itself and must read standalone. Never reference ADR numbers or other external document IDs in code comments (ADR discipline, see AGENTS.md); context documents may reference ADRs, code may not.
- State what the code does, never what it does not. Negative self-justification ("bytes never ride creation", "never a graph resource") is banned; a missing responsibility is documented by the interface that owns it.
- Use only vocabulary that already exists in the codebase — never invent terms. Engine threads are exactly Game/Render/RHI/Worker; an API thread contract says `any thread` or names a concrete thread ("producer/consumer" appears only in TaskGraph's task-supply sense).
- User-written comments (especially `// Note:`) are preserved verbatim: never delete or reword them unless the user asks (AGENTS.md Code Editing Rules).
