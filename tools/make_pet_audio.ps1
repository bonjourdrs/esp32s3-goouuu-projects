<#
.SYNOPSIS
    Regenerate the recorded replies for 06_offline_pet.

.DESCRIPTION
    Renders each phrase with the Windows TTS voice at the format the firmware
    expects (16 kHz, 16-bit, mono), then trims the leading and trailing silence
    so the clips stay small enough to embed in the application image.

.EXAMPLE
    .\tools\make_pet_audio.ps1
    .\tools\make_pet_audio.ps1 -Rate 3
#>
param(
    [string]$OutDir,
    [int]$Rate = 2,
    [string]$Voice = 'Microsoft Zira Desktop'
)

$ErrorActionPreference = 'Stop'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot '06_offline_pet\main\assets\wav'
}

$phrases = [ordered]@{
    'hello'       = 'I hear you!'
    'pickup'      = 'Hey! You picked me up.'
    'ouch'        = 'Ouch!'
    'shake_hard'  = "Stop it, I'm dizzy!"
    'upside_down' = "Whoa, I'm upside down!"
}

$rateHz = 16000
$threshold = 300
$leadPad = 50
$tailPad = 150

Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = $Rate
$synth.SelectVoice($Voice)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($name in $phrases.Keys) {
    $tmp = Join-Path $env:TEMP "$name.raw.wav"
    $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        $rateHz,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    $synth.SetOutputToWaveFile($tmp, $fmt)
    $synth.Speak($phrases[$name])
    $synth.SetOutputToNull()

    $bytes = [System.IO.File]::ReadAllBytes($tmp)

    $p = 12
    $dataOff = -1
    $dataLen = 0
    while ($p + 8 -le $bytes.Length) {
        $size = [BitConverter]::ToUInt32($bytes, $p + 4)
        if ($bytes[$p] -eq 0x64 -and $bytes[$p + 1] -eq 0x61 -and
            $bytes[$p + 2] -eq 0x74 -and $bytes[$p + 3] -eq 0x61) {
            $dataOff = $p + 8
            $dataLen = $size
            break
        }
        $p += 8 + $size + ($size % 2)
    }
    if ($dataOff -lt 0) { throw "no data chunk in $name" }

    $count = [int]($dataLen / 2)
    $first = -1
    $last = -1
    for ($i = 0; $i -lt $count; $i++) {
        $s = [BitConverter]::ToInt16($bytes, $dataOff + $i * 2)
        if ($s -gt $threshold -or $s -lt (-$threshold)) {
            if ($first -lt 0) { $first = $i }
            $last = $i
        }
    }
    if ($first -lt 0) { throw "$name rendered as silence" }

    $start = [Math]::Max(0, $first - $leadPad)
    $stop = [Math]::Min($count - 1, $last + $tailPad)
    $kept = $stop - $start + 1
    $pcmBytes = $kept * 2

    $out = New-Object byte[] (44 + $pcmBytes)
    [System.Text.Encoding]::ASCII.GetBytes('RIFF').CopyTo($out, 0)
    [BitConverter]::GetBytes([uint32](36 + $pcmBytes)).CopyTo($out, 4)
    [System.Text.Encoding]::ASCII.GetBytes('WAVE').CopyTo($out, 8)
    [System.Text.Encoding]::ASCII.GetBytes('fmt ').CopyTo($out, 12)
    [BitConverter]::GetBytes([uint32]16).CopyTo($out, 16)
    [BitConverter]::GetBytes([uint16]1).CopyTo($out, 20)
    [BitConverter]::GetBytes([uint16]1).CopyTo($out, 22)
    [BitConverter]::GetBytes([uint32]$rateHz).CopyTo($out, 24)
    [BitConverter]::GetBytes([uint32]($rateHz * 2)).CopyTo($out, 28)
    [BitConverter]::GetBytes([uint16]2).CopyTo($out, 32)
    [BitConverter]::GetBytes([uint16]16).CopyTo($out, 34)
    [System.Text.Encoding]::ASCII.GetBytes('data').CopyTo($out, 36)
    [BitConverter]::GetBytes([uint32]$pcmBytes).CopyTo($out, 40)

    [Array]::Copy($bytes, $dataOff + $start * 2, $out, 44, $pcmBytes)

    [System.IO.File]::WriteAllBytes((Join-Path $OutDir "$name.wav"), $out)
    Remove-Item $tmp -ErrorAction SilentlyContinue

    $ms = [Math]::Round($kept * 1000.0 / $rateHz)
    Write-Host ("{0,-16} {1,7} bytes  {2,5} ms" -f "$name.wav", $out.Length, $ms)
}

$synth.Dispose()
Write-Host "wrote 5 clips to $OutDir"
