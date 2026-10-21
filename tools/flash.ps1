<#
.SYNOPSIS
    Flash one of the example projects to the board.
.EXAMPLE
    .\tools\flash.ps1 -Project 01_blink_led -Port COM4
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

if ($Project -eq 'xiaozhi-esp32') {
    Write-Host "xiaozhi-esp32 is an independent app repo. Flash it inside its own directory:" -ForegroundColor Yellow
    Write-Host "  cd xiaozhi-esp32"
    Write-Host "  idf.py -p $Port flash monitor"
    exit 1
}
if ($Project -eq 'esp-ml307-src') {
    Write-Host "esp-ml307-src is a component source copy and has no standalone firmware. See knowledge/11-ml307-component.md" -ForegroundColor Yellow
    exit 1
}

. (Join-Path $PSScriptRoot 'env.ps1')

if (-not $BuildDir) {
    $root = if ($env:ESP32_BUILD_ROOT) { $env:ESP32_BUILD_ROOT } else { 'D:\esp32\build' }
    $BuildDir = Join-Path $root $Project
}

Push-Location $projPath
try {
    Write-Host "--- flash $Project to $Port ---"
    idf.py -B $BuildDir -p $Port flash
    Write-Host "flash exit=$LASTEXITCODE"
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
