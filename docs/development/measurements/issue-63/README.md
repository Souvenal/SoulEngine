# TaskGraph Incremental Build Comparison

- Issue: #63
- Baseline: #61, commit 83e10cc
- Branch: dev/issue-60
- Platform: macOS 26.6 arm64
- Compiler: Apple Homebrew Clang 23.1.3
- Build tool: xmake 3.1.1+20260827
- Configuration: xmake f -m debug

## Procedure

Repeated the #61 procedure: warm the build cache before each scenario, then perform three incremental measured runs. Each run changes one measurement-only token or value, builds TestsForTaskGraph_TaskGraph and SoulEngine, and restores or toggles the edit for the next run. No clean-build duration is treated as incremental evidence.

Commands: xmake build TestsForTaskGraph_TaskGraph; xmake build SoulEngine. Raw post-migration output is preserved beside this report. Baseline raw output remains in ../issue-61/.

## Results

All values are xmake-reported wall time in seconds. Runtime rows have no valid post-migration median because every run was blocked by a Clang frontend or ASTReader failure in the unchanged Launch module.

| Scenario | Target | Before median | After runs | After median | Rebuilt after migration |
| --- | --- | ---: | ---: | ---: | --- |
| Implementation-only | TestsForTaskGraph_TaskGraph | 2.055 | 0.927 / 0.976 / 0.961 | 0.961 | TaskGraph.cpp + test relink |
| Exported declaration/signature | TestsForTaskGraph_TaskGraph | 2.088 | 1.211 / 1.294 / 1.191 | 1.211 | BMI + TaskGraph.cppm + TaskGraph.cpp + test TU + relink |
| Public type definition | TestsForTaskGraph_TaskGraph | 2.036 | 1.193 / 1.244 / 1.157 | 1.193 | BMI + TaskGraph.cppm + TaskGraph.cpp + test TU + relink |
| Implementation-only | SoulEngine | 0.788 | unavailable: Launch frontend crash x3 | unavailable | stopped at Launch BMI |
| Exported declaration/signature | SoulEngine | 0.827 | unavailable: Launch frontend/ASTReader crash x3 | unavailable | stopped at Launch BMI |
| Public type definition | SoulEngine | 0.805 | unavailable: Launch frontend/ASTReader crash x3 | unavailable | stopped at Launch BMI |

The stronger result is the rebuilt-file set: implementation-only edits no longer rebuild the interface BMI, interface object, or importing test translation unit. Absolute timing is machine- and cache-state-dependent.

## Rebuild Observations

Implementation-only changed only the warning string in TaskGraph::Init inside TaskGraph.cpp. All three runs rebuilt only TaskGraph.cpp and the TestsForTaskGraph_TaskGraph link step. They did not rebuild TaskGraph.pcm, TaskGraph.cppm.o, or the test translation unit.

Exported declaration/signature renamed Init parameter WorkerCount to RequestedWorkerCount while preserving the function type and callers. All three runs rebuilt TaskGraph.pcm, TaskGraph.cppm.o, TaskGraph.cpp.o, the TaskGraph test TU, and the test link step.

Public type definition changed FrameTask::Sequence default from 0 to 1 and restored it. All three runs rebuilt the same BMI, interface object, implementation object, test TU, and link step.

## Before/After Artifact Evidence

| Artifact | Before | After |
| --- | ---: | ---: |
| TaskGraph.pcm | 20,429,008 bytes | 3,203,480 bytes |
| TaskGraph.cppm.o | 1,406,648 bytes | 134,936 bytes |
| TaskGraph.cpp.o | not present | 1,419,616 bytes |

Before migration, TaskGraph.cppm.o contained ordinary TaskGraph::Init, Enqueue, Drain, Shutdown, and WorkerLoop symbols. After migration those symbols are in TaskGraph.cpp.o; the interface object no longer contains those ordinary member definitions.

## Limitations

Post-migration SoulEngine runs cannot provide timing or downstream importer conclusions because Clang 23 repeatedly fails in unchanged Launch. Complete failure output is preserved in the *-runtime.log files. The TaskGraph test target is a direct importer and intentionally rebuilds on interface changes. The baseline and post-migration measurements were performed in different commits/worktrees, so compare rebuild sets first and timing second.

The invalid intermediate signature edit was discarded; the reported signature runs use a valid declaration/definition-only parameter rename.
