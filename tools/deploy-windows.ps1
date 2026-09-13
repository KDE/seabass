# Deploys seabass.exe as a standalone Windows build: runs windeployqt,
# then copies the *full* transitive DLL closure (Qt's own non-Qt deps, the
# mingw-w64 runtime, and every plugin's own dependencies -- notably the
# multimedia backend's FFmpeg codec libraries and the image-format plugins'
# codec libraries, neither of which windeployqt or a shallow one-off DLL list
# can predict). See docs/windows-build.md's "Deploying the GUI" section for
# why this can't just be a fixed list: it must be re-walked after any
# Qt/MSYS2 package update, since the exact set of transitive dependencies
# drifts with upstream package versions.
#
# Usage: run from anywhere, with MSYS2 UCRT64 installed at C:\msys64:
#   .\tools\deploy-windows.ps1 [-BuildDir build-win]

param(
    [string]$BuildDir = "build-win"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $RepoRoot $BuildDir
$UcrtBin = "C:\msys64\ucrt64\bin"
$env:PATH = "$UcrtBin;" + $env:PATH

$exePath = Join-Path $BuildPath "seabass.exe"
if (-not (Test-Path $exePath)) {
    throw "seabass.exe not found at $exePath -- build it first."
}

Write-Output "=== windeployqt ==="
# $ErrorActionPreference = "Stop" (above) makes PowerShell treat ANY stderr
# line from a native command as a terminating NativeCommandError, exit code
# irrelevant -- and windeployqt always writes one here ("Warning:
# Translations will not be available..."; no Qt Linguist tools in this
# MSYS2 setup, which is expected and harmless). That silently killed this
# script right after windeployqt, every run, before the DLL closure walk
# below -- the actual point of this script, and the one thing windeployqt
# itself does not do -- ever executed. Confirmed directly: seabass.exe
# built with taglib support (see docs/windows-build.md) crashed on launch
# from an installed build with "libtag-2.dll niet is gevonden" even though
# the closure walk's own logic would have found and copied it fine, because
# the walk never ran at all. Relaxed to Continue for just this one call,
# with the exit code checked explicitly instead -- a real windeployqt
# failure should still stop the script, a stderr warning must not.
$ErrorActionPreference = "Continue"
& windeployqt.exe --qmldir (Join-Path $RepoRoot "src\gui\qml") $exePath
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt.exe failed with exit code $LASTEXITCODE"
}
$ErrorActionPreference = "Stop"

Write-Output "`n=== walking full transitive DLL closure ==="
# Virtual API sets Windows itself resolves (never real files to copy) plus
# real system DLLs that ship with Windows -- anything NOT in these lists and
# not already found is assumed to be an MSYS2-provided DLL to copy across.
$ignorePrefixes = @("api-ms-win", "ext-ms-")
$ignoreExact = @(
    "KERNEL32.dll", "USER32.dll", "ADVAPI32.dll", "GDI32.dll", "ole32.dll", "OLEAUT32.dll", "SHELL32.dll",
    "WS2_32.dll", "BCRYPT.dll", "SECUR32.dll", "CRYPT32.dll", "VERSION.dll", "WINMM.dll", "ncrypt.dll",
    "d3d9.dll", "d3d11.dll", "d3d12.dll", "dwmapi.dll", "dxgi.dll", "dxva2.dll",
    "MF.dll", "MFPlat.DLL", "MFReadWrite.dll", "COMDLG32.dll", "IMM32.dll", "SHLWAPI.dll", "SHCORE.dll",
    "dwrite.dll", "d2d1.dll", "WINSPOOL.DRV", "uxtheme.dll", "NETAPI32.dll", "USERENV.dll", "MPR.dll",
    "CRYPTBASE.DLL", "bcryptprimitives.dll", "NTDLL.DLL", "RPCRT4.dll", "USP10.dll", "AUTHZ.dll", "AVRT.dll",
    "PROPSYS.dll", "DNSAPI.dll", "IPHLPAPI.DLL", "WINHTTP.dll", "SETUPAPI.dll", "WTSAPI32.dll", "MSIMG32.dll",
    "WSOCK32.dll", "gdiplus.dll"
)

function Get-Deps($dllPath) {
    (& objdump -p $dllPath 2>$null | Select-String "DLL Name") | ForEach-Object { ($_ -split "DLL Name: ")[1].Trim() }
}

$queue = [System.Collections.Generic.Queue[string]]::new()
Get-ChildItem -Path $BuildPath -Recurse -Filter "*.dll" | ForEach-Object { $queue.Enqueue($_.Name) }
Get-ChildItem -Path $BuildPath -Filter "*.exe" | ForEach-Object { $queue.Enqueue($_.Name) }

$copied = @()
$notFound = @()
while ($queue.Count -gt 0) {
    $name = $queue.Dequeue()
    $localPath = Get-ChildItem -Path $BuildPath -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $localPath) { continue }
    foreach ($d in Get-Deps $localPath.FullName) {
        if ($ignoreExact -contains $d) { continue }
        if ($ignorePrefixes | Where-Object { $d -like "$_*" }) { continue }
        if (Get-ChildItem -Path $BuildPath -Recurse -Filter $d -ErrorAction SilentlyContinue) { continue }
        $src = Join-Path $UcrtBin $d
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $BuildPath $d) -Force
            $copied += $d
            $queue.Enqueue($d)
        } else {
            $notFound += "$d (needed by $name)"
        }
    }
}

Write-Output "Copied $($copied.Count) DLL(s):"
$copied | Sort-Object -Unique | ForEach-Object { Write-Output "  $_" }
if ($notFound.Count -gt 0) {
    Write-Output "`nCould not resolve (verify these are genuinely part of the base OS, not a missing MSYS2 package):"
    $notFound | Sort-Object -Unique | ForEach-Object { Write-Output "  $_" }
}

# libsqlcipher-0.dll is loaded via LoadLibrary at runtime (see
# sqlcipher_dyn.hpp's doc comment), never appears in any import table, so
# the closure walk above can never find it -- copy it explicitly.
$sqlcipherSrc = Join-Path $UcrtBin "libsqlcipher-0.dll"
$sqlcipherDst = Join-Path $BuildPath "libsqlcipher-0.dll"
if ((Test-Path $sqlcipherSrc) -and -not (Test-Path $sqlcipherDst)) {
    Copy-Item $sqlcipherSrc $sqlcipherDst -Force
    Write-Output "`nAlso copied libsqlcipher-0.dll (runtime-loaded, never appears in any import table)."
}

# Without this, seabass.exe silently fails to load ANY QML at all once
# copied off this machine: QQmlApplicationEngine reported "module
# 'QtQuick' is not installed" (and every other Qt-provided QML module)
# even though windeployqt --qmldir already copied the whole qml\ tree
# right beside the exe. This build's Qt is not a relocatable install, so
# QLibraryInfo::path(QmlImportsPath) resolves to the *compile-time*
# absolute path (this machine's own C:\msys64\ucrt64\qml) instead of
# anything relative to the exe -- which is exactly why this went
# unnoticed: testing always happened on the machine MSYS2 is installed
# on, where that hardcoded path is also a real, valid answer, by
# accident, and the local qml\ copy next to the exe was never actually
# consulted. Confirmed directly: the app runs correctly once this
# qt.conf exists, and windeployqt does not write one on its own here.
$qtConfPath = Join-Path $BuildPath "qt.conf"
if (-not (Test-Path $qtConfPath)) {
    @"
[Paths]
Prefix = .
Plugins = .
Qml2Imports = qml
QmlImports = qml
"@ | Set-Content -Path $qtConfPath -Encoding ASCII
    Write-Output "`nAlso wrote qt.conf (without it, QML modules resolve to this machine's own MSYS2 install, not the bundled copy)."
}

Write-Output "`nDone. $exePath should now run standalone on a machine without MSYS2 installed."
