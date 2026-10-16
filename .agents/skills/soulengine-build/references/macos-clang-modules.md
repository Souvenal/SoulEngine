# macOS Clang C++ Modules

## Clang 23 drops dynamic initializers of unreferenced anonymous-namespace globals in named modules

**Diagnostic signature (runtime, not compile-time):** the build succeeds cleanly, but at startup every EnTT meta-registered component fails to resolve. Scene loading logs one warning per component:

    [warning] [ Main ] Default Scene warning at 'line 5, column 9': unknown component; component was omitted

No source changed; the only change was upgrading the toolchain (brew upgrade llvm, clang 22 -> 23). Both fresh debug and release builds are affected, so this is not a stale-BMI/cache issue.

**Root cause (verified with a minimal repro on clang 23.1.0, both -O0 and -O2):** a namespace-scope variable with a dynamic initializer inside an *anonymous namespace* in module purview gets no initializer at all when the variable is otherwise unreferenced — the constructor symbol and its __mod_init_func entry are silently dropped from the object file, and importers do not call the module initializer for it. The same variable in a *named* (module-linkage) namespace initializes correctly, and anonymous-namespace globals that *are* referenced from exported functions also initialize correctly. The same code compiled as a plain non-module TU also works. So the broken pattern is exactly: "registration object whose constructor has side effects, placed in an anonymous namespace of a .cppm module interface unit".

**Affected sites fixed (2026-08-31):** Core/ECS/Transform.cppm, Scene/Light.cppm, Scene/Camera.cppm, Scene/Mesh.cppm, Material/MaterialYamlLoader.cppm — the anonymous namespace wrapping each *MetaRegistration object was replaced with a named "namespace MetaRegistration" (with a // Note: comment explaining why).

**Verified repair:** give the registration object module linkage by naming the namespace. Minimal repro check after any future toolchain upgrade: a module whose anonymous-namespace Registration ctor sets a counter read via an exported getter; compile with --precompile + import; prints 0 when broken, 42 when correct.

**Verification commands:** rtk err xmake -y, then rtk err xmake test (TestsForScene_SceneDocument exercises the YAML -> entt::resolve path), and run the app to confirm zero "unknown component" warnings.

**Tempting but incorrect repairs:**
- xmake clean --all / deleting module caches — the failure survives a full rebuild; it is a codegen behavior change, not cache incoherence.
- Assuming only release builds are affected — the initializer is dropped at -O0 too.
- Worrying about referenced anonymous-namespace globals (e.g. Renderer.cppm g_RendererCache) — referenced globals still initialize; only unreferenced side-effect-registration objects are dropped.

## Clang 23 in-class ctor emission: an old failure, not a standing rule

**Historical report (2026-10-09), not reproducible in the current code shape.** A clean rebuild failed at link with an undefined symbol for a pass class's constructor, referenced from `std::make_unique<TPass>` inside a namespace-scope thunk template (`BuildPassFromStorage<TPass>`) instantiated in a consumer module:

    Undefined symbols for architecture arm64:
      "SoulEngine::BlitToSwapchainPass@RenderGraph::BlitToSwapchainPass(std::__1::unique_ptr<...Parameter...>)", referenced from:
          ... make_unique ... in RayTracingRenderer.cppm.o

The class (in-class, implicitly inline ctor) lives in `RenderGraph:BlitToSwapchain`; the defining TU never odr-uses the ctor, and the referencing instantiation was in another module. The repair applied then was an out-of-line ctor definition.

**Re-tested 2026-10-16 — the general rule does not hold.** `BuildPassFromStorage` no longer exists; construction is `std::make_unique<TPass>` directly inside `RenderGraphBuilder::AddPass`, a member function template instantiated in the consumer module — the same cross-module reference shape as the original report. With `BlitToSwapchainPass`'s ctor moved back in-class: clean rebuild (`xmake clean --all`, full compile of every module, ~240 s) linked the engine binary with no undefined symbols and `xmake test` reported 19/19 passed. Treat the old out-of-line rule as falsified for this code shape; the C++ coding spec's DRY rule also bans the declaration/definition split, so write ctors in-class unless a real link error says otherwise, and record the exact shape if one appears.

**Still worth checking in any future link failure:** which TU actually odr-uses the symbol (a consumer-module template instantiation is the historic suspect), and whether the missing symbol is a module-owned implicit-inline entity.
