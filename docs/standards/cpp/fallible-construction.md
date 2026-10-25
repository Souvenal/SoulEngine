# Fallible Construction

Read this document before designing a constructor or factory for an object whose creation can validate input, allocate memory, create an Engine resource, or otherwise fail.

## Construction Contract

A constructor represents guaranteed existence. A fallible object exposes a static `Create()` factory that returns `std::expected<..., ErrorMessage>`; callers inspect success before receiving a usable object. Two-phase construction creates an invalid interval between construction and `Init()`, so it is not an Engine construction pattern.

```cpp
class Resource {
public:
    static auto Create(const ResourceDesc& Desc, Dependencies... Deps)
        -> std::expected<Resource, ErrorMessage>;

private:
    Resource(Dependencies... Deps);
};
```

Use a normal constructor for infallible aggregates, math types, and dependency wiring that cannot fail.

## Factory Rules

1. Put the `*Desc` parameter first. It is the semantic payload; device handles, allocators, and contextual wiring follow.
2. Do not offer a public construction path that bypasses required fallible work. A constructor may be public only when a standard-library `make_shared` implementation requires it; document that it is a compatibility shim and direct callers to `Create()`.
3. Prefer value semantics. Return `std::expected<T, ErrorMessage>` for small movable values, `std::expected<SPtr<T>, ErrorMessage>` for polymorphic or shared-identity resources, and `std::expected<UPtr<T>, ErrorMessage>` for sole ownership.
4. A concrete `T::Create()` performs construction. An abstract interface factory delegates to concrete construction, adds interface-level context, and owns the relevant lifetime boundary; the two roles remain distinct.

## Review Questions

- Can this operation fail because of validation, allocation, external resource creation, or configuration? If so, does `Create()` return `std::expected`?
- Can a caller observe or use a partially initialized object? If so, redesign the API.
- Does the return ownership model match value, sole, or shared identity semantics?
- Does the API preserve declaration-site defaults and constructor-only infallible wiring?
