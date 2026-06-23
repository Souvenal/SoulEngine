# ADR 04 — Row-Major Host-to-Shader Matrix Convention

## Status

Accepted (2026-06-24)

## Context

SoulEngine uses Slang as the shader language and targets multiple graphics
backends over time. Matrix conventions sit on a boundary between CPU math code,
shader source, shader compiler options, and backend code generation. If those
layers disagree, transforms can silently become transposed or use the wrong
vector interpretation.

Slang's matrix-layout guidance identifies four variables that must be treated as
one convention: host vector interpretation, host matrix memory layout, shader
vector interpretation, and shader matrix memory layout. The same guide
recommends row-major layout with row-vector interpretation as the combination
that satisfies consistency, platform independence, and SIMD-friendly host math.
It also notes that some targets, including CPU/C++ and CUDA targets, do not
honor matrix layout settings and effectively use row-major layout.

Reference: [Slang — Handling Matrix Layout Differences on Different Platforms](https://shader-slang.org/slang/user-guide/a1-01-matrix-layout.html).

## Decision

SoulEngine adopts a row-major, row-vector convention for shader-visible
transform matrices.

This means:

- Slang compilation uses row-major matrix layout as the default.
- CPU-side shader-visible transform matrices are authored in row-major form.
- Shader transform code uses row-vector multiplication order:

```hlsl
float4 worldPos = float4(input.inPosition, 1.0);
float4 viewPos  = mul(worldPos, g_frame.cb.view);
float4 clipPos  = mul(viewPos, g_frame.cb.projection);
```

- CPU code does not routinely transpose matrices before upload.
- Runtime shaders do not use `column_major` / `row_major` annotations to patch
  ordinary engine transforms.
- If a future backend needs a conversion, that conversion belongs at an
  explicit backend or asset-boundary adapter, not as a per-shader layout fix.

The current CPU math library for shader-visible vector and matrix values is
`hlsl++`. It was chosen over GLM for this boundary because its default logical
layout and row-vector transform style align with Slang's recommended portable
row-major path. Other CPU math libraries may be used internally only if their
outputs are converted before crossing into shader-visible data.

## Consequences

This creates one canonical host-to-shader matrix contract. Reviewers should
check shader-visible transform changes for all of the following together:

- CPU matrix construction follows the engine's row-major convention.
- Slang compilation keeps row-major default matrix layout.
- Shader code uses `mul(vector, matrix)`, not `mul(matrix, vector)`.
- No ordinary runtime shader adds `column_major` to compensate for an upstream
  mismatch.

The convention improves portability because it follows the Slang-documented path
that works even when a target ignores matrix layout settings. It also avoids
hidden transpose work in the common path and keeps backend-specific layout
concerns out of runtime shader source.

The trade-off is that developers coming from OpenGL/GLM-style column-vector
examples must translate those examples into the engine convention. That cost is
intentional; preserving one convention at the shader-visible boundary is less
error-prone than allowing mixed local conventions.

## Non-Goals

This ADR is not about general buffer or vertex data layout. It only defines the
convention for shader-visible matrices and transform multiplication.

