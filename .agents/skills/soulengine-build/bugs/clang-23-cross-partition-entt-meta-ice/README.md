# Clang: cross-partition entt meta_factory mangling ICE

## 现象

模块分区 TU 的 PCM→OBJ codegen job 崩溃（`SEGV 0xC0000005`，frontend signal）。
BMI `--precompile` job 正常成功。崩溃栈：

```
0. Program arguments: clang++ -cc1 ... -emit-obj ...
1. <eof> parser at end of file
2. Per-file LLVM IR generation
3. .../entt/meta/factory.hpp:358:18: Mangling declaration
   'entt::meta_factory<SoulEngine::MaterialYamlRecord>::data'
```

## 触发条件

注册 TU 通过 `import :Types` 导入**同一模块另一分区**定义的类型 `T`，
并对 `T` 的成员实例化 `entt::meta_factory<T>::data<&T::Member>(...)`。
类型定义在注册 TU 本地时完全正常（repro 含对照组）。

## 影响版本

| 编译器 | 结果 |
| --- | --- |
| standalone LLVM 23.1.1 | 崩 |
| VS 2026 18 Community 自带 22.1.3 | 崩 |
| standalone 22.1.8 | 不崩（SoulEngine 当日全量构建实证） |

## 无效手段（全部实测仍崩）

拆分注册函数（崩溃转移到第一个含 `data<>` 的 helper）、`-O0`/`-O2`/`-O3`、
去掉 `-gline-tables-only`、clang-cl / clang++ 驱动切换、constexpr 成员指针中转变量。

## 规避（项目侧已落地）

entt meta 注册必须放在**定义该类型的同一个分区 TU**：MaterialYamlRecord 的注册
已从 `Material:YamlLoader` 迁到 `MaterialTypes.cppm`（`Material:Types`）。
或整体 pin standalone 22.1.8。注意注册块不能放进 `export namespace`
（`anonymous namespaces cannot be exported`），放文件尾部非导出命名空间。

## 复现步骤

```powershell
cd .agents/skills/soulengine-build/bugs/clang-23-cross-partition-entt-meta-ice/repro
./repro.ps1            # 或 ./repro.ps1 -Clang "C:/.../clang++.exe" -EnttInclude <path>
```

预期输出：`register codegen exit` 非 0（frontend signal），对照组
`selfregister codegen exit` 为 0。

## 上游状态

未提交。三份源文件 + 本 README 的触发矩阵可直接整理成 llvm-project issue。
