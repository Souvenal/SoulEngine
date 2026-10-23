# macOS Clang C++ Modules

## Clang 23 in-class ctor emission: an old failure, not a standing rule

**Historical report (2026-10-09), not reproducible in the current code shape.** A clean rebuild failed at link with an undefined symbol for a pass class's constructor, referenced from `std::make_unique<TPass>` inside a namespace-scope thunk template (`BuildPassFromStorage<TPass>`) instantiated in a consumer module:

    Undefined symbols for architecture arm64:
      "SoulEngine::BlitToSwapchainPass@RenderGraph::BlitToSwapchainPass(std::__1::unique_ptr<...Parameter...>)", referenced from:
          ... make_unique ... in RayTracingRenderer.cppm.o

The class (in-class, implicitly inline ctor) lives in `RenderGraph:BlitToSwapchain`; the defining TU never odr-uses the ctor, and the referencing instantiation was in another module. The repair applied then was an out-of-line ctor definition.

**Re-tested 2026-10-16 — the general rule does not hold.** `BuildPassFromStorage` no longer exists; construction is `std::make_unique<TPass>` directly inside `RenderGraphBuilder::AddPass`, a member function template instantiated in the consumer module — the same cross-module reference shape as the original report. With `BlitToSwapchainPass`'s ctor moved back in-class: clean rebuild (`xmake clean --all`, full compile of every module, ~240 s) linked the engine binary with no undefined symbols and `xmake test` reported 19/19 passed. Treat the old out-of-line rule as falsified for this code shape; the C++ coding spec's DRY rule also bans the declaration/definition split, so write ctors in-class unless a real link error says otherwise, and record the exact shape if one appears.

**Still worth checking in any future link failure:** which TU actually odr-uses the symbol (a consumer-module template instantiation is the historic suspect), and whether the missing symbol is a module-owned implicit-inline entity.
