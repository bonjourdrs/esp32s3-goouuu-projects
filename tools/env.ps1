# Shared toolchain environment for the GOOUUU ESP32-S3 projects.
# Dot-source it:  . "$PSScriptRoot\env.ps1"
# Every default below can be overridden by setting the matching
# environment variable before running a script.

if (-not $env:IDF_PATH)       { $env:IDF_PATH       = 'D:\Espressif\esp-idf' }
if (-not $env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = 'D:\Espressif\.espressif' }

# Portable MinGit is not on the system PATH, but idf.py needs a git there.
$minGit = 'D:\Espressif\MinGit\cmd'
if ((Test-Path $minGit) -and ($env:Path -notlike "*$minGit*")) {
    $env:Path = "$minGit;$env:Path"
}

. "$env:IDF_PATH\export.ps1"
