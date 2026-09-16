# Upstream compiler / toolchain bugs

本目录收录已定位到**编译器/工具链自身缺陷**的上游 bug：每个 bug 一个子目录，
包含 bug 现象说明（README.md）与最小复现材料（repro/ 或 reproducer/）。

与项目自身的构建配置问题区分：项目侧的规避手段记录在
[`../references/windows-clang-modules.md`](../references/windows-clang-modules.md)，
这里只放"值得报给上游"的东西。

## 索引

| 目录 | 一句话现象 | 影响版本 | 上游 |
| --- | --- | --- | --- |
| [`clang-23-cross-partition-entt-meta-ice/`](clang-23-cross-partition-entt-meta-ice/README.md) | 类型跨分区 import 后 mangle `meta_factory<T>::data` 即 SEGV | 23.1.1、22.1.3（22.1.8 不触发） | 未提交 |
| [`clang-23-instrumentation-pass-segv/`](clang-23-instrumentation-pass-segv/README.md) | 普通测试 TU 优化阶段插桩 pass SEGV（-O0 与 -O3 崩在不同的 pass） | 23.1.1 | 未提交 |

## 新增一个 bug 的约定

1. 目录名：`<编译器>-<核心签名>`，全小写连字符，例如 `clang-23-cross-partition-entt-meta-ice`。
2. `README.md` 固定小节：现象 / 触发条件 / 影响版本 / 无效手段 / 规避 / 复现步骤 / 上游状态。
3. 最小复现放 `repro/`（自写源文件 + 一键脚本）；clang 官方 crash reproducer 放 `reproducer/`。
   预处理后的巨型 .cpp（通常数 MB）不入库，README 里写再生成方式。
4. 项目侧规避落地后，在本表"规避"列引用落地位置（如 `Scene/xmake.lua`）。
