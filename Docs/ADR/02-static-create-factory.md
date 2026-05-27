# ADR 02 — Fallible Object Construction: Static Factory Over Constructor

**Status:** accepted

## Context

ADR 01 establishes two hard constraints:

1. **No exceptions.** Error handling uses `std::expected<T, ErrorMessage>` for
   every fallible function.
2. **Traditional constructors cannot return `std::expected`.**

When a constructor encounters failure, C++ offers exactly one built-in signal:
throw. Disable exceptions and the constructor has no way to report failure. The
caller gets an object and no indication of whether construction succeeded.

The initial workaround was two-phase initialization: a lightweight constructor
(wires dependencies, never fails) followed by a separate `Init()` method
(creates resources, returns `std::expected<void, ErrorMessage>`). This pattern
propagated through `Buffer`, `CommandList`, `Swapchain`, `TimelineSemaphore`,
and `RenderDevice`.

## Problem

Two-phase init has structural defects that compound as the codebase grows:

### 1. No compile-time guard

A constructed-but-uninitialized object compiles silently. Its internal handles
are `nullptr` or default values. Calling any operation on it is undefined
behavior with no static analysis warning. The error only surfaces at runtime.

### 2. Friction at every call site

Every construction is two statements where one should be:

```cpp
auto obj = std::make_shared<T>(dep1, dep2);
if (auto r = obj->Init(desc); !r)
    return std::unexpected(r.error());
```

Callers must remember both calls, in order, every time. The `Init()` call is
easily forgotten when adding new code under pressure.

### 3. Forbidden const-correctness

`Init` mutates the object after construction. Resource handle members cannot
be `const`. Every field carries a declaration-site default that is immediately
overwritten — a waste of initialization and a source of stale-default bugs.

### 4. Constructed state is fiction

A "constructed" `Buffer` with no VkBuffer allocation is not a buffer. It is a
hollow shell pretending to exist. The type system says `Buffer`; the reality
says "allocation allocation failed but nobody noticed because the constructor
can't say so."

## Decision

**Use a static `Create()` factory method for every object whose construction
can fail.** The factory returns `std::expected<Object, ErrorMessage>` where
`Object` is either the value itself or a pointer appropriate to ownership
semantics.

### Core convention

```cpp
class Resource {
public:
    static auto Create(const ResourceDesc& Desc, Dependencies... Deps)
        -> std::expected<Resource, ErrorMessage>;        // value
    // or
    static auto Create(const ResourceDesc& Desc, Dependencies... Deps)
        -> std::expected<SPtr<Resource>, ErrorMessage>;  // shared ownership
    // or
    static auto Create(const ResourceDesc& Desc, Dependencies... Deps)
        -> std::expected<UPtr<Resource>, ErrorMessage>;  // unique ownership

private:
    Resource(Dependencies... Deps);  // not public — use Create()
};
```

Rules:

1. **Descriptor first.** The `*Desc` parameter is always the first argument.
   It is the semantic payload — what to create. Device handles, allocators,
   and other contextual wiring follow.

2. **No public constructor for fallible types.** The constructor is `public`
   only when constrained by `std::make_shared` (see below). Even then, the
   public constructor is documented as a `make_shared` compatibility shim
   and callers are directed to `Create()`.

3. **`std::make_shared` compatibility.** Some libc++ implementations reject
   `make_shared` with private constructors. When that occurs, the constructor
   is made `public` with a doc comment directing all callers to `Create()`.
   This is a compiler workaround, not a design preference.

4. **Return value semantics when possible.**
   | Situation | Return type |
   |---|---|
   | Small movable value (~config, math, descriptors) | `std::expected<T, ErrorMessage>` |
   | Polymorphic / shared ownership / RHI resource | `std::expected<SPtr<T>, ErrorMessage>` |
   | Sole ownership, single holder | `std::expected<UPtr<T>, ErrorMessage>` |

   Default to value semantics. Use pointers only when the object's identity
   matters (GPU resources, window handles) or polymorphism requires indirection.

5. **Infallible construction stays with constructors.**
   ```cpp
   struct Vec3 { float x, y, z; };  // never fails → normal constructor
   ```
   This is not a blanket mandate to factory-wrap every type. Simple data
   aggregates, math types, and other infallible constructs use plain
   constructors.

### Relationship to ADR 01

ADR 01 observes that `std::expected` forces callers to consider failure cases.
This ADR extends that philosophy from *calling* a function to *constructing* an
object. Both directions — making errors visible, and making partial success
impossible — serve the same goal: correctness by construction at the type level.

### Factory vs. virtual dispatch

Static `Create()` methods are not virtual. This is not a defect — factory
functions and polymorphic interfaces serve different roles:

- `T::Create()` lives at the concrete level. It handles the mechanical work
  of construction and returns either success or failure.
- `RHI::RenderDevice::CreateBuffer()` lives at the abstract interface level.
  It delegates to the backend's `Create()`, wraps the error chain, manages
  resource lifetimes.

Both layers exist for different reasons. A static Create does not replace a
virtual factory; it makes the implementation side correct and the interface
side thinner.

### Constructor as no-fail guarantee

A constructor that cannot fail carries meaning: "this object's existence is
guaranteed." A factory that returns `expected` carries different meaning:
"this object might not exist — you must check." Both have their place. The
codebase should use each according to what it promises:

| Promise | Mechanism |
|---|---|
| Always succeeds | Constructor |
| May fail | `static auto Create() -> std::expected<...>` |

Two-phase init has no place in this table. It claims both promises and keeps
neither.

## Consequences

**Positive.**
- No object ever exists in partially-initialized state.
- Constructor == success. Caller never checks `if (!obj.isValid())`.
- Compile-time guarantee: `std::expected` must be inspected before use.
- Resource handles can be `const` — set once by `Create`, never changed.
- Single call site replaces two — lower surface area for ordering bugs.
- Pattern matches `std::expected` semantics established in ADR 01.

**Negative.**
- One extra line of boilerplate per fallible class (the static `Create`
  wrapper). Acceptable cost.
- Value-returning factories require the type to be movable.
- `std::make_shared` may force public constructors on some standard library
  implementations. Mitigated by doc comment and convention.
- Migration of existing two-phase init classes is churn. Phased migration
  — one class at a time — keeps the build green throughout.

**Neutral.**
- New contributors must learn one factory convention instead of one
  constructor + Init convention. Total learning surface is smaller.
