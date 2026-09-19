# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Exercises the built Windows installer end-to-end: silent install to an
# isolated directory, a smoke-run of seabass-cli.exe from that install,
# then silent uninstall -- with nothing left behind either way. Exits
# non-zero (and prints why) on any failure, so this is CI-callable as-is.
#
# Prerequisite: tools\windows-installer.iss already compiled into
# installer-out\Seabass-Setup-*.exe (see that script's own header comment
# for the full build -> deploy -> package sequence). Needs
# PrivilegesRequired=lowest in that script to install without an
# interactive UAC prompt -- without it, this hangs or aborts with Inno's
# "couldn't obtain elevation" exit code the moment there's no interactive
# desktop session to answer the prompt.
#
# Usage: run from the repo root:
#   .\tools\test-installer.ps1 [-InstallerDir installer-out] [-TestRoot <temp dir>]

param(
    [string]$InstallerDir = "installer-out",
    [string]$TestRoot = (Join-Path $env:TEMP "seabass-installer-test")
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
# Join-Path concatenates rather than substituting when the second arg is
# already rooted, producing a nonsense path -- only relevant when a caller
# overrides -InstallerDir with an absolute path, but worth getting right.
$InstallerPath = if ([System.IO.Path]::IsPathRooted($InstallerDir)) { $InstallerDir } else { Join-Path $RepoRoot $InstallerDir }

function Fail($message) {
    Write-Error $message
    exit 1
}

Write-Output "=== locating the built installer ==="
$installers = @(Get-ChildItem -Path $InstallerPath -Filter "Seabass-Setup-*.exe" -ErrorAction SilentlyContinue)
if ($installers.Count -eq 0) {
    Fail "No Seabass-Setup-*.exe found in $InstallerPath -- build and package first (see tools\windows-installer.iss)."
}
if ($installers.Count -gt 1) {
    Fail "Found $($installers.Count) installers in $InstallerPath -- expected exactly one. Clear stale ones out first: $($installers.Name -join ', ')"
}
$installerExe = $installers[0].FullName
Write-Output "Found: $installerExe"

if (Test-Path $TestRoot) {
    Remove-Item -Recurse -Force $TestRoot
}
$installDir = Join-Path $TestRoot "install"
$logPath = Join-Path $TestRoot "install.log"
New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null

Write-Output "`n=== silent install to $installDir ==="
$argString = "/VERYSILENT /SUPPRESSMSGBOXES /NOICONS /DIR=`"$installDir`" /LOG=`"$logPath`""
$proc = Start-Process -FilePath $installerExe -ArgumentList $argString -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    if (Test-Path $logPath) { Get-Content $logPath | Write-Output }
    Fail "Installer exited with code $($proc.ExitCode) (expected 0). If this is exit code 2 with no log at all, PrivilegesRequired=lowest is probably missing from windows-installer.iss -- see this script's header comment."
}
Write-Output "Installed cleanly."

Write-Output "`n=== smoke-testing seabass-cli.exe from the installed location ==="
$cliExe = Join-Path $installDir "seabass-cli.exe"
if (-not (Test-Path $cliExe)) {
    Fail "seabass-cli.exe is missing from the install -- $installDir"
}
# --help succeeds with a clean exit 0 and recognizable output. Deliberately
# not --version (no such flag): that falls through to help text too, but
# with a nonzero exit, which can't be told apart from a real failure --
# --help is the unambiguous signal that every DLL/plugin the exe needs
# actually resolved, not just that *some* text got printed.
$helpOutput = & $cliExe --help 2>&1
$helpExit = $LASTEXITCODE
if ($helpExit -ne 0) {
    Write-Output $helpOutput
    Fail "seabass-cli.exe --help exited with code $helpExit (expected 0) -- likely a missing DLL/plugin in the deployed closure. See docs/windows-build.md's 'Known gaps'."
}
if (-not ($helpOutput -match "seabass-cli")) {
    Write-Output $helpOutput
    Fail "seabass-cli.exe --help ran but didn't print recognizable help text -- something's off even though it exited 0."
}
Write-Output "seabass-cli.exe runs correctly from the installed location."

Write-Output "`n=== silent uninstall ==="
$uninstaller = Join-Path $installDir "unins000.exe"
if (-not (Test-Path $uninstaller)) {
    Fail "No uninstaller found at $uninstaller"
}
$proc = Start-Process -FilePath $uninstaller -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES" -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    Fail "Uninstaller exited with code $($proc.ExitCode) (expected 0)."
}
Start-Sleep -Milliseconds 500  # unins000.exe can return slightly before its own directory is gone
if (Test-Path $installDir) {
    Fail "Uninstall reported success but $installDir still exists."
}
Write-Output "Uninstalled cleanly, nothing left behind."

Remove-Item -Recurse -Force $TestRoot -ErrorAction SilentlyContinue
Write-Output "`nAll installer checks passed."
