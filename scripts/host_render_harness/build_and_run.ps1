# Build and run the host render harness (MSVC x86), then pixel-verify
# the dumps against scripts/render_shr4.py.
#
#   powershell -File scripts\host_render_harness\build_and_run.ps1
#
# Compiles the REAL apple_cycle_renderer.c / appletini_ntsc.c for the
# host and executes the T1/T2/T3/T4/T5 scenarios (see harness.c). 32-bit x86
# so hardware-address arithmetic on uint32_t stays pointer-sized.

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$work = Join-Path $env:TEMP "appletini_host_harness"
New-Item -ItemType Directory -Force "$work\build", "$work\out" | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$vcvars = "$vs\VC\Auxiliary\Build\vcvars32.bat"

$sources = @(
    "$repo\scripts\host_render_harness\harness.c",
    "$repo\ps_sources\frontend\apple_cycle_renderer.c",
    "$repo\ps_sources\frontend\appletini_ntsc.c",
    "$repo\ps_sources\frontend\appletini_csbits.c",
    "$repo\ps_sources\frontend\apple2e_video_rom_data.c"
) -join " "

cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /FI $repo\scripts\host_render_harness\harness_prefix.h /I $repo\scripts\host_render_harness /I $repo\scripts\host_render_harness\xil_stub /I $repo\ps_sources\frontend /Fo$work\build\ /Fe$work\build\harness.exe $sources"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

$shortRoot = Join-Path $work "short asset root"
New-Item -ItemType Directory -Force $shortRoot | Out-Null
$shortAsset = Join-Path $shortRoot "truncated asset.bin"
[System.IO.File]::WriteAllBytes($shortAsset, [byte[]](0..31))
& "$work\build\harness.exe" --check-exact-file `
    $shortRoot "truncated asset.bin" 0x8000
if ($LASTEXITCODE -eq 0) {
    throw "harness accepted a truncated fixed-size asset"
}

& "$work\build\harness.exe" $repo "$work\out"
if ($LASTEXITCODE -ne 0) { throw "harness reported failures" }

python "$repo\scripts\host_render_harness\check_output.py" "$work\out"
if ($LASTEXITCODE -ne 0) { throw "pixel checker reported failures" }

Write-Host "host render harness: ALL PASS"
