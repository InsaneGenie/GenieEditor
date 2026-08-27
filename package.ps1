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

# libmpv, copied explicitly. CMake's TARGET_RUNTIME_DLLS only knows about
# imported targets, and mpv is linked as a bare path to mpv.lib -- so nothing in
# the build ever learns that libmpv-2.dll needs to travel with the binary. It is
# present in the Debug tree only because it was put there by hand, which is
# exactly the kind of thing that works locally and fails for every user.
$mpvDir = Split-Path -Parent $MpvLibrary
$mpvSearchDirs = @(
    $mpvDir
    (Join-Path $mpvDir "bin")
    (Join-Path $mpvDir "..\bin")
    (Split-Path -Parent $mpvDir)
    (Join-Path $Root "build\Debug")   # last resort: wherever it was put by hand
)
$mpvDlls = @()
foreach ($dir in $mpvSearchDirs) {
    if ($dir -and (Test-Path $dir)) {
        $mpvDlls += @(Get-ChildItem $dir -Filter "*mpv*.dll" -ErrorAction SilentlyContinue)
    }
}
# Still nothing: search the whole mpv folder tree before giving up.
if (-not $mpvDlls -and (Test-Path $mpvDir)) {
    $mpvDlls = @(Get-ChildItem $mpvDir -Filter "*mpv*.dll" -Recurse -ErrorAction SilentlyContinue)
}
if (-not $mpvDlls) {
    Write-Host "  looked in:"
    $mpvSearchDirs | ForEach-Object { Write-Host "    $_" }
}
if ($mpvDlls) {
    $mpvDlls | Select-Object -Unique | ForEach-Object {
        Copy-Item $_.FullName $Staging -Force
        Write-Host "  copied $($_.Name)"
    }
} else {
    Write-Warning "libmpv-2.dll NOT FOUND near $MpvLibrary - the app will not start without it."
    Write-Warning "Find it and copy it into the staging folder by hand."
}

# windeployqt catches anything the CMake step missed - Qt's own dependency graph
# is deep enough that hand-listing DLLs eventually gets one wrong.
# vcpkg moves this between layouts (tools/Qt6/bin, tools/qt6/bin, and a separate
# debug tree), so it is searched for rather than assumed. $VcpkgTools is tried
# first so an explicit override at the top of this script still wins.
$WinDeployQt = $null
$candidates = @(
    (Join-Path $VcpkgTools "windeployqt.exe")
    "C:\Users\steve\vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.exe"
    "C:\Users\steve\vcpkg\installed\x64-windows\tools\qt6\bin\windeployqt.exe"
)
foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { $WinDeployQt = $c; break }
}
if (-not $WinDeployQt) {
    $found = Get-ChildItem "C:\Users\steve\vcpkg\installed" -Filter "windeployqt.exe" -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -notmatch "\\debug\\" } |
             Select-Object -First 1
    if ($found) { $WinDeployQt = $found.FullName }
}

if ($WinDeployQt) {
    Write-Host "  windeployqt: $WinDeployQt"
    Write-Host "  running windeployqt"
    $deployArgs = @(
        "--release"
        "--no-translations"
        "--no-system-d3d-compiler"
        (Join-Path $Staging "GenieEditor.exe")
    )
    & $WinDeployQt @deployArgs | Out-Null
} else {
    Write-Warning "windeployqt.exe not found - Qt DLLs may be missing from the package."
    Write-Warning "CMake copies most of them, so test the zip on another machine before trusting it."
}

# Qt plugins, from the RELEASE tree. CMake copies these too, but it only learned
# to pick the right tree per configuration just now -- and a package that
# silently omits platforms/qwindows.dll produces a startup failure whose error
# message mentions neither Qt nor plugins. Cheap to do twice, expensive to miss.
$qtPluginRoot = "C:\Users\steve\vcpkg\installed\x64-windows\Qt6\plugins"
if (Test-Path $qtPluginRoot) {
    foreach ($sub in @("platforms", "imageformats", "tls", "styles")) {
        $src = Join-Path $qtPluginRoot $sub
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $Staging $sub) -Recurse -Force
            Write-Host "  copied Qt $sub plugins"
        }
    }
} else {
    Write-Warning "Qt release plugins not found at $qtPluginRoot"
}

# OpenSSL, for HTTPS. Qt loads its TLS backend as a PLUGIN at runtime, and that
# plugin in turn loads libssl/libcrypto -- so neither is a link-time dependency
# of the executable and TARGET_RUNTIME_DLLS never learns about them. Without
# them Qt silently falls back to its "cert-only" backend, which cannot open a
# TLS connection at all: the app starts and looks fine, but the GIF and Sounds
# panels can never load anything. This is the exact failure windeployqt exists
# to prevent, which is why it has to be done by hand here.
Write-Host "`n=== 3d/5  OpenSSL ===" -ForegroundColor Cyan
$vcpkgBin = "C:\Users\steve\vcpkg\installed\x64-windows\bin"
$sslCopied = @()
foreach ($name in @("libssl-3-x64.dll", "libcrypto-3-x64.dll")) {
    if (Test-Path (Join-Path $Staging $name)) { $sslCopied += $name; continue }
    $src = Join-Path $vcpkgBin $name
    if (-not (Test-Path $src)) {
        $found = Get-ChildItem "C:\Users\steve\vcpkg\installed" -Filter $name -Recurse -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notmatch "\\debug\\" } |
                 Select-Object -First 1
        if ($found) { $src = $found.FullName }
    }
    if (Test-Path $src) {
        Copy-Item $src $Staging -Force
        $sslCopied += $name
        Write-Host "  copied $name"
    } else {
        Write-Warning "$name NOT FOUND - HTTPS will not work, so the GIF and Sounds panels will fail."
    }
}

# The Visual C++ runtime. A Release build links msvcp140.dll and vcruntime140*.dll
# dynamically. Most Windows machines already have them from some other
# application, but not all do, and when they don't the app fails to start with a
# missing-DLL box that names a Microsoft file and explains nothing. Shipping them
# app-locally is explicitly permitted for the RELEASE runtime (unlike the debug
# one) and removes the whole class of problem.
Write-Host "`n=== 3b/5  Visual C++ runtime ===" -ForegroundColor Cyan
# A Release build links msvcp140.dll and vcruntime140*.dll dynamically. Most
# Windows machines have them from some other application, but not all, and when
# they don't the app fails to start with a missing-DLL box naming a Microsoft
# file and explaining nothing. Shipping them app-locally is explicitly permitted
# for the RELEASE runtime (unlike the debug one).
#
# Globbed rather than walked, because the redist path contains two version
# numbers that change with every Visual Studio update and a recursive search of
# Program Files is slow and permission-prone.
$crtDlls = @()
$globs = @(
    "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT\*.dll"
    "C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT\*.dll"
)
foreach ($g in $globs) {
    $crtDlls += Get-ChildItem $g -ErrorAction SilentlyContinue
}

if (-not $crtDlls) {
    # Last resort: the copies already installed on this machine. Same
    # redistributable files, just already unpacked.
    Write-Host "  redist folder not found, taking the installed copies instead"
    $crtDlls = Get-ChildItem "C:\Windows\System32" -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -match "^(msvcp140.*|vcruntime140.*|concrt140)\.dll$" }
}

if ($crtDlls) {
    $crtDlls | Sort-Object Name -Unique | ForEach-Object {
        Copy-Item $_.FullName $Staging -Force
    }
    Write-Host "  copied $(($crtDlls | Sort-Object Name -Unique).Count) Visual C++ runtime DLLs"
} else {
    Write-Warning "Visual C++ runtime DLLs not found. The app will still run on machines"
    Write-Warning "that already have them - link https://aka.ms/vs/17/release/vc_redist.x64.exe"
    Write-Warning "on your download page."
}

# The itch.io app scans a package for executables and offers every one it finds
# as something to launch -- so the bundled ffmpeg.exe appears alongside the
# editor, and being the larger file it sorts first. A manifest names the single
# real entry point and the picker disappears.
#
# The leading dot matters: the app looks for exactly ".itch.toml" in the root of
# the extracted folder.
Write-Host "`n=== 3e/5  itch.io manifest ===" -ForegroundColor Cyan
$manifest = @'
# Tells the itch.io app what to launch. Without this it finds the bundled
# ffmpeg.exe as well and asks the user to choose, which is confusing and easy
# to get wrong.
[[actions]]
name = "play"
path = "GenieEditor.exe"
'@
$manifestPath = Join-Path $Staging ".itch.toml"
# Written without a BOM: the app's TOML parser treats a leading byte-order mark
# as part of the first key and fails to find the actions table.
[System.IO.File]::WriteAllText($manifestPath, $manifest.Replace("`r`n", "`n"),
                               (New-Object System.Text.UTF8Encoding($false)))
Write-Host "  wrote .itch.toml pointing at GenieEditor.exe"

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

# Strip debug artefacts that the build may have deposited alongside the release
# output. Two kinds, both pure waste in a shipped package:
#
#   *.pdb            debug symbol files. Never loaded at runtime, and Qt's are
#                    enormous -- qwindowsd.pdb alone runs to tens of megabytes.
#   plugins ending d Qt's debug naming convention (qwindowsd.dll beside
#                    qwindows.dll). They need the debug Qt libraries and the
#                    debug CRT, neither of which ships here, so they could only
#                    ever fail to load.
Write-Host "`n=== 3c/5  Removing debug artefacts ===" -ForegroundColor Cyan
$freed = 0

$pdbs = @(Get-ChildItem $Staging -Recurse -Filter "*.pdb" -ErrorAction SilentlyContinue)
foreach ($pdb in $pdbs) {
    $freed += $pdb.Length
    Remove-Item $pdb.FullName -Force
}
if ($pdbs) { Write-Host "  removed $($pdbs.Count) .pdb files" }

# Only removed when the RELEASE twin is present, so a legitimately d-suffixed
# name can never be deleted out from under the app.
$debugPlugins = @()
foreach ($dll in @(Get-ChildItem $Staging -Recurse -Filter "*d.dll" -ErrorAction SilentlyContinue)) {
    $releaseTwin = Join-Path $dll.DirectoryName ($dll.BaseName.Substring(0, $dll.BaseName.Length - 1) + ".dll")
    if (Test-Path $releaseTwin) { $debugPlugins += $dll }
}
foreach ($dll in $debugPlugins) {
    $freed += $dll.Length
    Remove-Item $dll.FullName -Force
}
if ($debugPlugins) { Write-Host "  removed $($debugPlugins.Count) debug-build plugins" }
Write-Host ("  reclaimed {0:N1} MB" -f ($freed / 1MB))

# ffmpeg dominates the download if the wrong build is bundled. Worth saying so
# once rather than leaving it to be discovered from the finished zip size.
$ffmpeg = Join-Path $Staging "ffmpeg.exe"
if (Test-Path $ffmpeg) {
    $ffMb = (Get-Item $ffmpeg).Length / 1MB
    if ($ffMb -gt 100) {
        Write-Warning ("ffmpeg.exe is {0:N0} MB - that is the 'full' gyan.dev build." -f $ffMb)
        Write-Warning "The 'essentials' build is a fraction of the size and has everything"
        Write-Warning "this app uses. See the note in THIRD-PARTY.md."
    }
}

# Nothing below here can fix a missing DLL, so the package is checked while
# there is still something to look at. These are the files whose absence stops
# the app from starting at all, as opposed to degrading a feature.
$required = @("GenieEditor.exe", "libmpv-2.dll", "Qt6Core.dll", "Qt6Widgets.dll",
              "Qt6Gui.dll", "Qt6Network.dll", "whisper.dll",
              # HTTPS needs all three. Missing any one degrades quietly to a
              # TLS backend that cannot connect, rather than failing loudly.
              "libssl-3-x64.dll", "libcrypto-3-x64.dll",
              "tls\qopensslbackend.dll",
              # Without this the itch app asks the user which exe to run.
              ".itch.toml")
# @() forces an array. Where-Object returns a bare string when exactly one item
# matches, and += on a string concatenates rather than appends -- which is why
# the previous run reported "libmpv-2.dllplatforms\qwindows.dll" as one name.
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $Staging $_)) })
if (-not (Test-Path (Join-Path $Staging "platforms\qwindows.dll"))) {
    $missing += "platforms\qwindows.dll"
}
if ($missing) {
    $missing | ForEach-Object { Write-Warning "MISSING: $_" }
    throw "Required files are missing - the package would not run on a user's machine"
}
Write-Host "  all required files present" -ForegroundColor Green

Write-Host "`n=== 5/5  Zipping ===" -ForegroundColor Cyan
$Zip = Join-Path $Root "dist\GenieEditor-$Version-win64.zip"
if (Test-Path $Zip) { Remove-Item $Zip }

# Built entry by entry rather than with Compress-Archive.
#
# Compress-Archive writes entry paths using the platform separator, so on
# Windows every path inside the zip contains backslashes. The ZIP specification
# requires forward slashes. Most extractors tolerate it, but the itch.io app
# uses a strict one that reports "zip: insecure file path" and refuses to
# install -- a failure that only shows up for people installing through the app,
# never for anyone testing a browser download.
Add-Type -AssemblyName System.IO.Compression | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$stagingRoot = (Resolve-Path $Staging).Path
$folderName  = Split-Path $Staging -Leaf
$zipStream = [System.IO.File]::Create($Zip)
$archive = New-Object System.IO.Compression.ZipArchive(
    $zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    # -Force so nothing with a hidden attribute is skipped. A leading dot does
    # not hide a file on Windows, but .itch.toml is the one entry whose absence
    # would be invisible until someone installed through the app.
    foreach ($file in Get-ChildItem $Staging -Recurse -File -Force) {
        $relative = $file.FullName.Substring($stagingRoot.Length).TrimStart('\', '/')
        # The one line that matters: forward slashes, always.
        $entryName = ($folderName + '/' + $relative) -replace '\\', '/'
        $entry = $archive.CreateEntry($entryName,
                                      [System.IO.Compression.CompressionLevel]::Optimal)
        # Not $input / $output: $input is a PowerShell automatic variable, and
        # reusing it works at script scope but breaks in a pipeline or function.
        $inStream = [System.IO.File]::OpenRead($file.FullName)
        $outStream = $entry.Open()
        try { $inStream.CopyTo($outStream) } finally { $outStream.Dispose(); $inStream.Dispose() }
    }
} finally {
    $archive.Dispose()
    $zipStream.Dispose()
}

# Verified rather than assumed, because the symptom appears only in the itch
# app and would otherwise be found by users rather than here.
$check = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $bad = @($check.Entries | Where-Object { $_.FullName -match '\\' })
    if ($bad) {
        $bad | Select-Object -First 3 | ForEach-Object { Write-Warning "BACKSLASH ENTRY: $($_.FullName)" }
        throw "Zip contains backslash paths - the itch app would refuse to install this"
    }
    Write-Host "  $($check.Entries.Count) entries, all with forward-slash paths"
} finally { $check.Dispose() }

$sizeMb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host "`nDone: $Zip  ($sizeMb MB)" -ForegroundColor Green
Write-Host "Before uploading, unzip it somewhere else and run it there - that is"
Write-Host "the only way to catch a missing DLL, since your own machine has them all."