# Clang 23.1.1: instrumentation pass SEGV on a plain test TU

## 现象

`Engine/Source/Runtime/Scene/Tests/SceneDocument.cpp`（普通 .cpp 测试 TU：
文本包含 gtest/entt/hlsl++，`import Scene;`）在 clang 23.1.1 下编译时前端
SEGV（`0xC0000005`，frontend signal）。崩溃发生在**优化管线**，且换优化级别
换 pass：

| -O 级别 | 崩溃 pass |
| --- | --- |
| -O2 / -O3 | `memprof-remove-attributes` |
| -O0 | `function(ee-instrument<>)` |

即无论怎么调优化级别都躲不开；两个都是 LLVM 23 新管线的插桩相关 pass。

## 触发条件

与 SoulEngine 代码无关——同一 TU 的官方 crash reproducer 已随库保存
（`reproducer/SceneDocument-757a3d.sh`，26KB）。复现要素：C++23 具名模块
消费者 TU（`import Scene;` 连带 import Material/Core/std），文本包含
entt/hlsl++，MSVC 目标三件套 `-fms-runtime-lib=dll -fvisibility=hidden -O2`。
引擎其它几十个同构测试 TU 全部正常，仅此 TU 触发（对 LLVM 的 IR 形状敏感）。

## 影响版本

| 编译器 | 结果 |
| --- | --- |
| standalone LLVM 23.1.1 | 崩 |
| 22.1.x | 未测（两个崩溃 pass 均为 23 新管线，22 理论上不涉及） |

## 无效手段

-O0 / -O2 / -O3 切换（见上表，换个 pass 继续崩）。

## 规避（项目侧已落地）

`Scene/xmake.lua` 的 `test_module` 调用按工具链 `exclude_tests` 排除
该测试（`is_config("toolchain", "clang", "llvm")` 时排除）。LLVM 升级过
23.1.1 并确认修复后移除排除。

## 复现步骤

方式一（走完整构建）：

```powershell
xmake f -c --toolchain=clang -y
rtk err xmake test    # TestsForScene_SceneDocument 编译阶段 SEGV
```

方式二（直接用 reproducer，需要先有 build 目录里的 Scene.pcm）：
`reproducer/SceneDocument-757a3d.sh` 是 clang 官方 crash reproducer 脚本，
在仓库根目录用 bash 执行即可复现（脚本内引用仓库相对路径的 PCM 与
Temp 目录的预处理 .cpp）。

再生成 reproducer（Temp 目录被清理后）：`xmake -v -y` 重触发崩溃时 clang
会重新在 `%TEMP%` 落 `SceneDocument-xxxx.cpp/.sh` 对。

## 上游状态

未提交。附官方 reproducer 脚本即可成稿；两个崩溃 pass 名与 -O 无关性是
关键描述点。
