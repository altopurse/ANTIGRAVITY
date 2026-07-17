# Builds dist\AntigravityVoiceEngine-Setup.exe - the single file to ship.
# Run AFTER building:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   powershell -ExecutionPolicy Bypass -File package.ps1
# Requires Inno Setup:  winget install JRSoftware.InnoSetup

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$exe = Join-Path $root "build\bin\voice-changer.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Binary not found at $exe. Build first: cmake --build build"
    exit 1
}

# ---------------------------------------------------------------------------
# Code signing (optional but strongly recommended - removes the SmartScreen
# "unknown publisher" wall). Configured entirely via environment variables so
# no secrets ever live in the repo. Two modes (see docs/code-signing.md):
#
#   1) PFX certificate file:
#        CODESIGN_PFX          = C:\path\to\cert.pfx
#        CODESIGN_PFX_PASSWORD = <password>
#   2) Azure Trusted Signing (or any custom pipeline):
#        CODESIGN_CMD = full command line; {file} is replaced with the target
#        e.g. signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com
#             /td SHA256 /dlib "...\Azure.CodeSigning.Dlib.dll"
#             /dmdf "...\metadata.json" {file}
#
# If neither is set, packaging proceeds unsigned with a reminder.
# NOTE: the exe is signed BEFORE the installer packs it and before the
# anti-tamper self-hash is computed, so the shipped hash matches what users run.
# ---------------------------------------------------------------------------
function Invoke-CodeSign {
    param([string]$Path)
    if ($env:CODESIGN_CMD) {
        $cmd = $env:CODESIGN_CMD.Replace("{file}", '"' + $Path + '"')
        Write-Output "Signing (custom cmd): $Path"
        cmd /c $cmd
        if ($LASTEXITCODE -ne 0) { Write-Error "Code signing failed for $Path"; exit 1 }
        return
    }
    if ($env:CODESIGN_PFX) {
        $signtool = (Get-Command signtool -ErrorAction SilentlyContinue).Source
        if (-not $signtool) {
            # Fall back to the newest Windows SDK copy
            $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
        }
        if (-not $signtool) { Write-Error "signtool.exe not found (install the Windows 10/11 SDK)"; exit 1 }
        Write-Output "Signing (PFX): $Path"
        & $signtool sign /f $env:CODESIGN_PFX /p $env:CODESIGN_PFX_PASSWORD `
            /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $Path
        if ($LASTEXITCODE -ne 0) { Write-Error "Code signing failed for $Path"; exit 1 }
        return
    }
    Write-Output "NOTE: shipping UNSIGNED ($([System.IO.Path]::GetFileName($Path))) - users will hit SmartScreen."
    Write-Output "      See docs/code-signing.md to set up signing (~10 GBP/month, big conversion win)."
}

Invoke-CodeSign -Path $exe

# Fresh dist folder
$dist = Join-Path $root "dist"
if (Test-Path $dist) {
    Remove-Item -Recurse -Force $dist
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "sounds") | Out-Null

# Generate the two sample soundboard clips (16-bit mono PCM WAV, pure PowerShell).
# They're compile inputs for the installer (setup.iss packs dist\sounds\*).
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

if (-not $iscc) {
    Write-Error "Inno Setup not found. Install it first: winget install JRSoftware.InnoSetup"
    exit 1
}

Write-Output "Compiling installer with Inno Setup..."
& $iscc /Qp (Join-Path $root "installer\setup.iss")
if ($LASTEXITCODE -ne 0) {
    Write-Error "Inno Setup compilation failed."
    exit 1
}

# Sign the installer itself too (the file users actually download and run -
# this is what SmartScreen judges first).
Invoke-CodeSign -Path (Join-Path $dist "AntigravityVoiceEngine-Setup.exe")

# The sounds were only needed as compile inputs; ship a clean single file.
Remove-Item -Recurse -Force (Join-Path $dist "sounds")

# Publish the installer into the website (server/public/downloads/): the
# live site at /download always serves whatever was committed here last.
$publishDir = Join-Path $root "server\public\downloads"
New-Item -ItemType Directory -Force -Path $publishDir | Out-Null
Copy-Item -Force (Join-Path $dist "AntigravityVoiceEngine-Setup.exe") -Destination $publishDir

# Anti-tamper: compute the exe's self-hash (same FNV-1a the app reports on
# /api/verify as &h=). Set this as EXPECTED_HASH in Render for THIS release so
# the dashboard can flag any machine reporting a different (patched) binary.
$exeForHash = Join-Path $root "build\bin\voice-changer.exe"
$expectedHash = ""
$nodeExe = (Get-Command node -ErrorAction SilentlyContinue).Source
if ($nodeExe -and (Test-Path $exeForHash)) {
    $expectedHash = & node -e "const fs=require('fs');let h=1469598103934665603n;const p=1099511628211n;const m=(1n<<64n)-1n;for(const b of fs.readFileSync(process.argv[1])){h=(h^BigInt(b))&m;h=(h*p)&m;}console.log(h.toString(16).padStart(16,'0'));" "$exeForHash"
}

Write-Output "=================================================="
Write-Output "Package ready: $dist\AntigravityVoiceEngine-Setup.exe"
Write-Output "Also copied to: $publishDir\AntigravityVoiceEngine-Setup.exe"
if ($expectedHash) {
    Write-Output ""
    Write-Output "Anti-tamper: set this in Render for this release ->"
    Write-Output "  EXPECTED_HASH = $expectedHash"
}
Write-Output ""
Write-Output "To publish this build on the website:"
Write-Output "  git add server/public/downloads/AntigravityVoiceEngine-Setup.exe"
Write-Output "  git commit -m `"Publish vX.Y.Z installer`""
Write-Output "  git push origin main   (Render auto-deploys)"
Write-Output "=================================================="
