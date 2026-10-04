# Builds version.dll (the drop-in hook) and test_locator.exe (offline self-test).
# Uses the MinGW-w64 g++ already on PATH. No Visual Studio or Qt needed.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null

$common = @("-std=c++17", "-O2")

Write-Host "Building version.dll ..."
& g++ @common -shared -static -static-libgcc -static-libstdc++ `
    -o (Join-Path $build "version.dll") `
    (Join-Path $root "src/dllmain.cpp") `
    (Join-Path $root "src/locator.cpp") `
    (Join-Path $root "src/listener.cpp") `
    (Join-Path $root "src/version.def") `
    -lws2_32
if ($LASTEXITCODE -ne 0) { throw "version.dll build failed" }

Write-Host "Building test_locator.exe ..."
& g++ @common -municode `
    -o (Join-Path $build "test_locator.exe") `
    (Join-Path $root "src/test_locator.cpp") `
    (Join-Path $root "src/locator.cpp")
if ($LASTEXITCODE -ne 0) { throw "test_locator build failed" }

Write-Host ""
Write-Host "Done. Output in: $build"
Write-Host "  version.dll        -> copy into your RPCS3 folder (next to rpcs3.exe)"
Write-Host "  test_locator.exe   -> optional: run against rpcs3.exe to check symbol detection"
