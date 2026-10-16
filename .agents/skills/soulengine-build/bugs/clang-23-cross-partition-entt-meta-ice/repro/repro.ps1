# Reproduces the clang cross-partition entt meta mangling ICE.
# Expected: case 1 step 3 exits nonzero with a frontend signal (SEGV);
#           case 2 (control) compiles clean end to end.
# Verified crashing: standalone 23.1.1, VS 2026 bundled 22.1.3.
# Verified clean:    standalone 22.1.8.
param(
    [string]$Clang = "clang++",
    [string]$EnttInclude = "$env:USERPROFILE/.local/share/xmake/packages/e/entt/v4.0.0/0d58f676872d486790799bfc6f016197/include"
)

$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
$Common = @("--target=x86_64-pc-windows-msvc", "-std=c++23", "-O2")

Write-Host "== case 1: cross-partition registration (expected crash at codegen) =="
& $Clang @Common -x c++-module --precompile bugrepro_types.cppm -o bugrepro_types.pcm
Write-Host "types precompile exit=$LASTEXITCODE"
& $Clang @Common -x c++-module --precompile "-fmodule-file=bugrepro:Types=bugrepro_types.pcm" "-I$EnttInclude" bugrepro_register_crash.cppm -o bugrepro_register_crash.pcm
Write-Host "register precompile exit=$LASTEXITCODE"
& $Clang @Common "-fmodule-file=bugrepro:Types=bugrepro_types.pcm" -c bugrepro_register_crash.pcm
Write-Host "register codegen exit=$LASTEXITCODE  <- nonzero (signal) = bug reproduced"

Write-Host "== case 2: control, type defined in the registering TU (expected clean) =="
& $Clang @Common -x c++-module --precompile "-I$EnttInclude" bugrepro_selfregister_ok.cppm -o bugrepro_selfregister_ok.pcm
Write-Host "selfregister precompile exit=$LASTEXITCODE"
& $Clang @Common -c bugrepro_selfregister_ok.pcm
Write-Host "selfregister codegen exit=$LASTEXITCODE  <- zero = control passes"
