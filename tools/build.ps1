<#
.SYNOPSIS
    Build one of the example projects.
.EXAMPLE
    .\tools\build.ps1 -Project 01_blink_led
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$BuildDir
)

$ErrorActionPreference = 'Continue'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$repoRoot = Split-Path -Parent $PSScriptRoot
$projPath = Join-Path $repoRoot $Project
if (-not (Test-Path $projPath)) { throw "Project not found: $projPath" }

. (Join-Path $PSScriptRoot 'env.ps1')

# Short external build dir keeps Windows' MAX_PATH limit out of the way.
if (-not $BuildDir) {
    $root = if ($env:ESP32_BUILD_ROOT) { $env:ESP32_BUILD_ROOT } else { 'D:\esp32\build' }
    $BuildDir = Join-Path $root $Project
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Push-Location $projPath
try {
    Write-Host "--- set-target esp32s3 : $Project ---"
    idf.py -B $BuildDir set-target esp32s3
    Write-Host "--- build : $Project ---"
    idf.py -B $BuildDir build
    Write-Host "build exit=$LASTEXITCODE"
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
