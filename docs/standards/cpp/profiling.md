# Tracy Profiling

Read this document when adding or changing Tracy frame marks, zones, or thread names.

## Stable Names

Tracy identifies event strings by pointer address. Define a name once as an `inline constexpr const char*` and pass that variable to `FrameMarkStart`, `FrameMarkEnd`, `FrameMarkNamed`, `ZoneScopedN`, or `tracy::SetThreadName`. Do not reuse fresh literals at separate instrumentation sites. Indexed names passed to `tracy::SetThreadName` may be formatted locals because that API copies its input.

## Frame and Zone Conventions

- Thread names use `<Domain>Thread`, such as `GameThread`, `RenderThread`, and `RHIThread`.
- Continuous frame spans use `<Domain>Frame`, such as `GameFrame`, `RenderFrame`, and `RHIFrame`.
- A zone records work; do not add a top-level zone that duplicates a frame span.
- Emit the unnamed master `FrameMark` on the RHI thread immediately after `EndFrame` so Tracy's default numbered frame set represents submit and present.
- GPU contexts use timestamp calibration through `TracyVulkan` or equivalent backend context; GPU zones do not depend on the master frame mark.
