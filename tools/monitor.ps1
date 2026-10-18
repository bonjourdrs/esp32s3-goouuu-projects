<#
.SYNOPSIS
    Open the serial monitor for one of the example projects.
.NOTES
    Press Ctrl+] to exit the monitor.
.EXAMPLE
    .\tools\monitor.ps1 -Project 01_blink_led -Port COM4
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Port = 'COM4',
    [string]$BuildDir
)

$ErrorActionPreference = 'Continue'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$repoRoot = Split-Path -Parent $PSScriptRoot
$projPath = Join-Path $repoRoot $Project
if (-not (Test-Path $projPath)) { throw "Project not found: $projPath" }

. (Join-Path $PSScriptRoot 'env.ps1')

if (-not $BuildDir) {
    $root = if ($env:ESP32_BUILD_ROOT) { $env:ESP32_BUILD_ROOT } else { 'D:\esp32\build' }
    $BuildDir = Join-Path $root $Project
}

Push-Location $projPath
try {
    Write-Host "--- monitor $Project on $Port (Ctrl+] to quit) ---"
    idf.py -B $BuildDir -p $Port monitor
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
