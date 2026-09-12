<#
.SYNOPSIS
    Creates a "JUa9 Bridge" shortcut on the current user's Desktop that
    launches JUa9Bridge.exe with no arguments (normal tray-icon mode).

.DESCRIPTION
    Run this once after building the project. It looks for
    JUa9Bridge.exe next to this script's repo (walking up from
    scripts\ to the project root, then into the usual x64 build output
    folders), or you can pass -ExePath explicitly if your build output
    lives somewhere else.

.PARAMETER ExePath
    Full path to JUa9Bridge.exe. If omitted, the script searches the
    common Debug/Release x64 output folders.

.EXAMPLE
    .\CreateDesktopShortcut.ps1

.EXAMPLE
    .\CreateDesktopShortcut.ps1 -ExePath "C:\path\to\JUa9Bridge.exe"
#>

param(
    [string]$ExePath
)

$ErrorActionPreference = "Stop"

if (-not $ExePath) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        Join-Path $repoRoot "x64\Debug\JUa9Bridge.exe"
        Join-Path $repoRoot "x64\Release\JUa9Bridge.exe"
        Join-Path $repoRoot "Debug\JUa9Bridge.exe"
        Join-Path $repoRoot "Release\JUa9Bridge.exe"
    )
    $ExePath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $ExePath) {
        Write-Error "Couldn't find JUa9Bridge.exe in the usual build output folders. Build the project first, or pass -ExePath explicitly."
        exit 1
    }
}

$ExePath = (Resolve-Path $ExePath).Path
$workingDir = Split-Path -Parent $ExePath
$desktop = [Environment]::GetFolderPath("Desktop")
$shortcutPath = Join-Path $desktop "JUa9 Bridge.lnk"

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $ExePath
$shortcut.WorkingDirectory = $workingDir
$shortcut.IconLocation = "$ExePath,0"
$shortcut.Description = "Start the JUa9 vJoy bridge (runs in the system tray)"
$shortcut.Save()

Write-Host "Created shortcut: $shortcutPath"
Write-Host "Target: $ExePath"
