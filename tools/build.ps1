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

if ($Project -eq 'xiaozhi-esp32') {
    Write-Host "xiaozhi-esp32 is an independent app repo. Build it with its own script:" -ForegroundColor Yellow
    Write-Host "  cd xiaozhi-esp32"
    Write-Host "  python scripts/build.py goouuu-s3-dual"
    exit 1
}
if ($Project -eq 'esp-ml307-src') {
    Write-Host "esp-ml307-src is a component source copy and is not built standalone. See knowledge/11-ml307-component.md" -ForegroundColor Yellow
    exit 1
}

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
