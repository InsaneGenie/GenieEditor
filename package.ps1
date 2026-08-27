# Builds a RELEASE zip of GenieEditor ready to upload.
#
# Run from the repository root:
#     powershell -ExecutionPolicy Bypass -File .\package.ps1
#
# WHY RELEASE AND NOT DEBUG
#
# A Debug build links the debug C runtime (ucrtbased.dll, vcruntime140d.dll and
# friends). Microsoft does not permit redistributing those, and they are only
# present on machines with Visual Studio installed - so a Debug build shipped to
# users fails to start with a missing-DLL error on almost every machine that
# isn't yours. This is the single most common way a first Windows release goes
# wrong, and the failure gives no hint about the cause.

$ErrorActionPreference = "Stop"

$Root    = $PSScriptRoot
$Build   = Join-Path $Root "build-release"
$Version = "0.1.0"
$Staging = Join-Path $Root "dist\GenieEditor-$Version-win64"

# Adjust if your toolchain lives elsewhere.
$Toolchain  = "C:\Users\steve\vcpkg\scripts\buildsystems\vcpkg.cmake"
$MpvInclude = "C:\libmpv\include"
$MpvLibrary = "C:\libmpv\mpv.lib"
$VcpkgTools = "C:\Users\steve\vcpkg\installed\x64-windows\tools\Qt6\bin"

Write-Host "`n=== 1/5  Configuring (Release) ===" -ForegroundColor Cyan
# A separate build directory from your Debug one, so day-to-day debugging isn't
# disturbed by packaging and vice versa.
# Arguments are built as an array and splatted rather than written inline with
# backtick continuations. A bare -DNAME=$Var token is not reliably expanded by
# PowerShell, and when it isn't, the literal text "$Toolchain" is handed to
# cmake -- which then reports a missing toolchain file by that name. Quoting
# each element makes expansion unambiguous.
$configureArgs = @(
    "-S", $Root
    "-B", $Build
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
    "-DMPV_INCLUDE_DIR=$MpvInclude"
    "-DMPV_LIBRARY=$MpvLibrary"
    "-DCMAKE_BUILD_TYPE=Release"
)

# Fail early and clearly if a path is wrong, rather than letting cmake report it
# in its own terms several lines later.
foreach ($p in @($Toolchain, $MpvInclude, $MpvLibrary)) {
    if (-not (Test-Path $p)) { throw "Path not found: $p  (fix the variables at the top of this script)" }
}

Write-Host "  toolchain: $Toolchain"
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "Configure failed" }

Write-Host "`n=== 2/5  Building ===" -ForegroundColor Cyan
cmake --build $Build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$ExeDir = Join-Path $Build "Release"
$Exe    = Join-Path $ExeDir "GenieEditor.exe"
if (-not (Test-Path $Exe)) { throw "GenieEditor.exe not found at $Exe" }

Write-Host "`n=== 3/5  Staging ===" -ForegroundColor Cyan
if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
New-Item -ItemType Directory -Force -Path $Staging | Out-Null

# CMakeLists already copies the runtime DLLs, Qt plugins and ffmpeg.exe next to
# the binary as a post-build step, so the whole output directory is the payload.
Copy-Item -Recurse -Force (Join-Path $ExeDir "*") $Staging

# windeployqt catches anything the CMake step missed - Qt's own dependency graph
# is deep enough that hand-listing DLLs eventually gets one wrong.
$WinDeployQt = Join-Path $VcpkgTools "windeployqt.exe"
if (Test-Path $WinDeployQt) {
    Write-Host "  running windeployqt"
    $deployArgs = @(
        "--release"
        "--no-translations"
        "--no-system-d3d-compiler"
        (Join-Path $Staging "GenieEditor.exe")
    )
    & $WinDeployQt @deployArgs | Out-Null
} else {
    Write-Warning "windeployqt not found at $WinDeployQt - verify Qt DLLs are present by hand"
}

Write-Host "`n=== 4/5  Licences and docs ===" -ForegroundColor Cyan
foreach ($f in @("LICENSE", "THIRD-PARTY.md", "README.md")) {
    $src = Join-Path $Root $f
    if (Test-Path $src) { Copy-Item $src $Staging }
    else { Write-Warning "$f is missing - it is REQUIRED when distributing under the GPL" }
}

# Nothing debug-flavoured should ever reach a user. Caught here rather than
# trusted, because the symptom on their machine is an unexplained failure to
# start.
$debugDlls = Get-ChildItem $Staging -Recurse -Filter "*d.dll" |
             Where-Object { $_.Name -match "(ucrtbased|vcruntime\d+d|msvcp\d+d)\.dll" }
if ($debugDlls) {
    $debugDlls | ForEach-Object { Write-Warning "DEBUG RUNTIME PRESENT: $($_.Name)" }
    throw "Debug runtime DLLs found - this build would not start on a user's machine"
}

Write-Host "`n=== 5/5  Zipping ===" -ForegroundColor Cyan
$Zip = Join-Path $Root "dist\GenieEditor-$Version-win64.zip"
if (Test-Path $Zip) { Remove-Item $Zip }
Compress-Archive -Path $Staging -DestinationPath $Zip

$sizeMb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host "`nDone: $Zip  ($sizeMb MB)" -ForegroundColor Green
Write-Host "Before uploading, unzip it somewhere else and run it there - that is"
Write-Host "the only way to catch a missing DLL, since your own machine has them all."