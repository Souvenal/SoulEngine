# TaskGraph Incremental Build Baseline

- Issue: [#61](https://github.com/Souvenal/SoulEngine/issues/61)
- Commit: `dc6db0a2cbc113b4f146b029e2326427e17290aa`
- Platform: macOS 26.6, arm64
- Compiler: Apple Homebrew Clang 23.1.3
- Build tool: xmake 3.1.1+20260827
- Configuration: `xmake f -m debug`
- Dependencies: `xmake repo -u` completed; xmake resolved project packages from the local package cache. Key observed versions: GoogleTest 1.17.0, Tracy 0.14.1, magic_enum 0.9.8, Vulkan headers 1.4.357+0.

## Procedure

The baseline was measured in the isolated `dev/issue-60` worktree based on fetched `origin/main`. Build cache was warmed for both targets before each scenario. Each measured run changed one interface token/value, built the TaskGraph test target and runtime target to completion, and the next run changed it back or forward. This keeps every sample an incremental build from a completed prior build; no clean-build duration is included.

Targets and commands:

- TaskGraph plus module test: `xmake build TestsForTaskGraph_TaskGraph`
- Representative runtime/downstream target: `xmake build SoulEngine`

Three measured scenarios:

- Implementation-body control: changed only the warning string in `TaskGraph::Init` between two same-length descriptions. This is an implementation token change, though the body remains in the interface in the baseline.
- Exported declaration/signature control: renamed the `Init` parameter from `WorkerCount` to `RequestedWorkerCount`. The exported spelling changes while the function type and callers remain valid.
- Public type control: changed `FrameTask::Sequence` default from `0` to `1` and back. This affects a public type definition; it is measurement-only and was reverted.

One warm-up and three measured runs were completed per scenario. Wall time below is xmake's reported `spent` duration; complete raw output is preserved beside this report. Values are seconds.

| Scenario | Target | Run 1 | Run 2 | Run 3 | Median |
| --- | --- | ---: | ---: | ---: | ---: |
| Implementation-only | TestsForTaskGraph_TaskGraph | 2.056 | 2.055 | 2.047 | 2.055 |
| Implementation-only | SoulEngine | 0.788 | 0.798 | 0.784 | 0.788 |
| Exported declaration | TestsForTaskGraph_TaskGraph | 2.058 | 2.102 | 2.088 | 2.088 |
| Exported declaration | SoulEngine | 0.827 | 0.842 | 0.791 | 0.827 |
| Public type definition | TestsForTaskGraph_TaskGraph | 2.026 | 2.062 | 2.036 | 2.036 |
| Public type definition | SoulEngine | 0.828 | 0.805 | 0.803 | 0.805 |

## Rebuild Observations

All three edit types caused xmake to rebuild the TaskGraph module BMI, compile `Engine/Source/Runtime/TaskGraph/TaskGraph.cppm`, compile the TaskGraph test translation unit, and relink `TestsForTaskGraph_TaskGraph`. The `SoulEngine` target only relinked in the captured runs; its module importers did not recompile for these specific experiments. This means this baseline does not demonstrate the desired implementation-only isolation: the ordinary method bodies are in the primary module interface, and xmake/Clang treat the module interface as a dependency of the test consumer. A separate downstream consumer that imports TaskGraph may need a dedicated focused target to make propagation visible in this experiment.

The signature control trace is in `signature-rebuild-trace.log`; per-run build output is retained in the `*-runN-*.log` files. The trace records BMI generation/compilation and test translation-unit compilation. Runtime traces show linking only.

## Limitations

- The implementation-only sample changes a string literal in the interface body. This is the closest safe body-only control on the unmodified source; it cannot establish post-migration behavior because the function is still in the interface.
- The TaskGraph test target depends on the whole TaskGraph interface, so it rebuilds on interface edits and is not a pure downstream importer-only measure.
- The runtime target was already up to date except for relinking in these runs. Follow-up #63 should add or identify representative module consumer targets and compare their module compilation units after migration.
- Build timing is machine-specific. Compare before and after on this same machine/configuration; do not interpret these absolute values as a general compile-time guarantee.
- An attempted signature control that changed the parameter type failed at link because the unchanged runtime caller referenced the old function type. That invalid trial was discarded; the accepted signature control preserves the function type.
- An attempted temporary extra data member failed compilation and was discarded. The accepted public type control changes a default member initializer only.

All temporary edits were reverted. The TaskGraph source matches the baseline snapshot saved as `TaskGraph.cppm.baseline`.
