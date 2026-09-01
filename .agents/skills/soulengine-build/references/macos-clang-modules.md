# macOS Clang C++ Modules

## Clang 23 drops dynamic initializers of unreferenced anonymous-namespace globals in named modules

**Diagnostic signature (runtime, not compile-time):** the build succeeds cleanly, but at
startup every EnTT meta-registered component fails to resolve. Scene loading logs one
warning per component:

    [warning] [ Main ] Default Scene warning at 'line 5, column 9': unknown component; component was omitted

No source changed; the only change was upgrading the toolchain (brew upgrade llvm,
clang 22 -> 23). Both fresh debug and release builds are affected, so this is not a
stale-BMI/cache issue.

**Root cause (verified with a minimal repro on clang 23.1.0, both -O0 and -O2):**
a namespace-scope variable with a dynamic initializer inside an *anonymous namespace*
in module purview gets no initializer at all when the variable is otherwise
unreferenced — the constructor symbol and its __mod_init_func entry are silently
dropped from the object file, and importers do not call the module initializer for it.
The same variable in a *named* (module-linkage) namespace initializes correctly, and
anonymous-namespace globals that *are* referenced from exported functions also
initialize correctly. The same code compiled as a plain non-module TU also works.
So the broken pattern is exactly: "registration object whose constructor has side
effects, placed in an anonymous namespace of a .cppm module interface unit".

**Affected sites fixed (2026-08-31):**
Core/ECS/Transform.cppm, Scene/Light.cppm, Scene/Camera.cppm,
Scene/Mesh.cppm, Material/MaterialYamlLoader.cppm — the anonymous namespace
wrapping each *MetaRegistration object was replaced with a named
"namespace MetaRegistration" (with a // Note: comment explaining why).

**Verified repair:** give the registration object module linkage by naming the
namespace. Minimal repro check after any future toolchain upgrade: a module whose
anonymous-namespace Registration ctor sets a counter read via an exported getter;
compile with --precompile + import; prints 0 when broken, 42 when correct.

**Verification commands:** rtk err xmake -y, then rtk err xmake test
(TestsForScene_SceneDocument exercises the YAML -> entt::resolve path), and run the
app to confirm zero "unknown component" warnings.

**Tempting but incorrect repairs:**
- xmake clean --all / deleting module caches — the failure survives a full rebuild;
  it is a codegen behavior change, not cache incoherence.
- Assuming only release builds are affected — the initializer is dropped at -O0 too.
- Worrying about referenced anonymous-namespace globals (e.g. Renderer.cppm
  g_RendererCache) — referenced globals still initialize; only unreferenced
  side-effect-registration objects are dropped.
