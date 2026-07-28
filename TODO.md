# TODO

## Verify Windows plain `clang` after the xmake CMake-path quoting fix

- [ ] After installing an xmake update that contains the fix for xmake issue `xmake-io/xmake#7663` (PR `xmake-io/xmake#7668`, merged July 26, 2026), verify that `C:\Program Files\LLVM` resource paths no longer split while CMake-built packages are linked with the Windows plain `clang` toolchain.
  - Confirm `modules/package/tools/cmake.lua` contains `_quote_flags_with_spaces`.
  - Reproduce the standalone `XmakeMinizipRepro` project from a clean configuration:

    ```powershell
    rtk err xmake clean --all -y
    rtk err xmake f -c -m debug -y
    rtk err xmake -y
    ```

  - Then verify SoulEngine:

    ```powershell
    rtk err xmake clean --all -y
    rtk err xmake f -m debug -y
    rtk err xmake -y
    rtk err xmake test -v
    ```

  - Record whether `minizip` installs successfully and whether the generated CMake/Ninja link command preserves LLVM paths containing spaces.