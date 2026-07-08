# Builds the distributable "dist" folder from the compiled binary and the
# installer sources. Run AFTER building:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   powershell -ExecutionPolicy Bypass -File package.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$exe = Join-Path $root "build\bin\voice-changer.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Binary not found at $exe. Build first: cmake --build build"
    exit 1
}

# Fresh dist folder
$dist = Join-Path $root "dist"
if (Test-Path $dist) {
    Remove-Item -Recurse -Force $dist
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "sounds") | Out-Null

# Copy binary + installer files
Write-Output "Copying binary and installer files..."
Copy-Item -Force $exe -Destination $dist
Copy-Item -Force (Join-Path $root "installer\*") -Destination $dist

# Generate the two sample soundboard clips (16-bit mono PCM WAV, pure PowerShell)
function New-ToneWav {
    param(
        [string]$Path,
        [double]$Freq,
        [double]$Duration
    )
    $sr = 44100
    $n = [int]($Duration * $sr)
    $dataSize = $n * 2

    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    try {
        $bw.Write([byte[]][char[]]"RIFF")
        $bw.Write([int](36 + $dataSize))
        $bw.Write([byte[]][char[]]"WAVE")
        $bw.Write([byte[]][char[]]"fmt ")
        $bw.Write([int]16)          # fmt chunk size
        $bw.Write([int16]1)         # PCM
        $bw.Write([int16]1)         # mono
        $bw.Write([int]$sr)         # sample rate
        $bw.Write([int]($sr * 2))   # byte rate
        $bw.Write([int16]2)         # block align
        $bw.Write([int16]16)        # bits per sample
        $bw.Write([byte[]][char[]]"data")
        $bw.Write([int]$dataSize)

        for ($i = 0; $i -lt $n; $i++) {
            $t = $i / $sr
            # Sine with exponential decay so it sounds like a pleasant chime
            $s = [math]::Sin(2 * [math]::PI * $Freq * $t) * [math]::Exp(-3 * $t)
            $bw.Write([int16][math]::Round(32000 * $s))
        }
    }
    finally {
        $bw.Close()
    }
}

Write-Output "Generating sample sounds..."
New-ToneWav -Path (Join-Path $dist "sounds\chime.wav") -Freq 587.33 -Duration 1.2
New-ToneWav -Path (Join-Path $dist "sounds\beep.wav")  -Freq 880.0  -Duration 0.4

# Build the single-file setup wizard (AntigravityVoiceEngine-Setup.exe)
# Requires Inno Setup: winget install JRSoftware.InnoSetup
$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($iscc) {
    Write-Output "Compiling installer with Inno Setup..."
    & $iscc /Qp (Join-Path $root "installer\setup.iss")
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Inno Setup compilation failed."
        exit 1
    }
    Write-Output "Installer: $dist\AntigravityVoiceEngine-Setup.exe"
} else {
    Write-Warning "Inno Setup not found - skipping single-exe installer. (winget install JRSoftware.InnoSetup)"
}

Write-Output "=================================================="
Write-Output "Package ready in: $dist"
Write-Output "Give users AntigravityVoiceEngine-Setup.exe (one file, ready to run)."
Write-Output "(The folder with Install.bat still works as a script-based fallback.)"
Write-Output "=================================================="
