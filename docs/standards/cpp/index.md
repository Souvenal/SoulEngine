# C++ Coding Specifications

Read this index for every C++ change. Read the linked topic when its trigger matches the work.

## High-frequency rules

- Apply `[[nodiscard]]` to functions with return values.
- RTTI is disabled (`-fno-rtti`). Use `std::variant`, `std::visit`, or virtual dispatch for type-erased polymorphism.
- Do not write raw `new` or `delete`. Use `UPtr<T>` for sole ownership and `SPtr<T>` for shared ownership. Raw pointers are non-owning observers. C API allocation belongs in an RAII wrapper with a `// NOLINT-RAW-MEM` explanation.
- Use project aliases from `Core:Util.Types`. `UPtr<T>` and `SPtr<T>` are enforced; `Int32`, `String`, `StringView`, and `ErrorMessage` are preferred.
- Use aggregate initialization (`MyStruct{ .Field = Value }`) for structs. Use `= {}` when every field keeps its default.
- Apply trailing return types to free functions, non-constructor/non-destructor members, operators, and lambdas.
- Prefix non-static `class` data members with `m_`; struct and static data members have no prefix. Give `bool` members an `Is` prefix.
- Define each function once. Ordinary non-template module members may be declared in an interface and defined once in its attached implementation unit. Mutually recursive free functions may use a documented `ALLOW-FWD-RECURSION` declaration.
- Write data-member defaults at the declaration site. Use constructor initializer lists for infallible value assignment and dependency injection.
- Start every `enum class` with `Unknown = 0` or an equivalent zero sentinel.
- Comment rules live in [Comments](../comments.md).

## Triggered references

- **Error paths:** when a function can fail or forwards `ErrorMessage`, read [Error handling](error-handling.md).
- **Fallible construction:** when construction can validate, allocate, create an Engine resource, or otherwise fail, read [Fallible construction](fallible-construction.md) before designing the constructor or factory API.
- **C++ modules:** when creating, splitting, exporting, or moving a module or partition, read [Modules](modules.md).
- **Tracy:** when adding or changing frame marks, zones, or thread names, read [Profiling](profiling.md).

## Quick Reference

| Rule | Scope | Enforced |
|------|-------|----------|
| `[[nodiscard]]` on return values | All functions | Static analysis |
| Explicit error returns | Fallible functions | Code generation / review |
| `-fno-rtti` | All code | Compiler flag |
| No raw `new` / `delete` | All code | Review |
| Trailing return types | Functions except ctor/dtor | Review |
| One definition | All functions | Review |
| `m_` prefix | `class` non-static members | Review |
| Aggregate initialization | Structs | Review |
| Declaration-site defaults | All data members | Review |
| `UPtr<T>` / `SPtr<T>` aliases | All code | Review |
| Zero sentinel | All `enum class` | Review |
| Tracy stable names | Tracy macros | Review |
