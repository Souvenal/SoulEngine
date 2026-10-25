# Error Handling

Read this document when a function can fail, when forwarding an `ErrorMessage`, or when deciding whether a condition is an error or warning.

## Rules

- SoulEngine uses no exceptions. A fallible function returns `std::expected<T, ErrorMessage>`; a fallible void operation returns `std::expected<void, ErrorMessage>`.
- An error prevents normal continuation through that operation. The callee returns `ErrorMessage`; the terminal caller formats it with `.ToString()` and enters crash-handling flow.
- A warning allows continuation. Log it in the callee with `LogWarning`; do not create an `std::expected` solely to report a warning.
- A leaf failure returns `std::unexpected(ErrorMessage(...))`. A forwarding layer adds context with `.Append()`.
- Human-readable errors and logs stringify enums with `magic_enum::enum_name()`, not numeric casts.

## Examples

`ErrorMessage` and `.Append()` take `String`. Use `Format()` for dynamic text.

```cpp
return std::unexpected(ErrorMessage("TOML parse error at line 42"));
return std::unexpected(ErrorMessage(Format("Failed to load '{}'", FilePath)));
return std::unexpected(Result.error().Append("Failed to load config file"));
return std::unexpected(Result.error().Append(Format("Failed to load '{}'", FilePath)));
LogError("{}", Result.error().ToString());
```

A tool or review rule reports its rule ID and a suggested rewrite. Unsafe automatic exception migration reports `RULE-NO-EXCEPTIONS` with a manual patch.
