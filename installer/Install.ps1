# Antigravity Voice Engine - Windows Installer (user-space, no admin required)
# Run via Install.bat, or:  powershell -ExecutionPolicy Bypass -File Install.ps1

$ErrorActionPreference = "Stop"

# Resolve script directory robustly
if ($PSCommandPath -and $PSCommandPath.Trim() -ne "") {
    $scriptDir = Split-Path -Parent $PSCommandPath
} elseif ($PSScriptRoot -and $PSScriptRoot.Trim() -ne "") {
    $scriptDir = $PSScriptRoot
} else {
    $scriptDir = (Get-Location).ProviderPath
    Write-Warning "Script path not available. Using current directory as scriptDir: $scriptDir"
}

# 1. Setup paths
$appName = "AntigravityVoiceEngine"
$appDisplayName = "Antigravity Voice Engine"
$installFolder = Join-Path $env:LOCALAPPDATA $appName
$desktopPath = [System.Environment]::GetFolderPath("Desktop")
$startMenuPath = [System.Environment]::GetFolderPath("StartMenu")
$programsPath = Join-Path $startMenuPath "Programs"

$exeSource = Join-Path $scriptDir "voice-changer.exe"
if (-not (Test-Path $exeSource)) {
    Write-Error "voice-changer.exe not found next to this script ($exeSource). Run this installer from the extracted dist folder."
    exit 1
}

Write-Output "=================================================="
Write-Output "Installing $appDisplayName..."
Write-Output "Target: $installFolder"
Write-Output "=================================================="

try {
    # 2. Stop running instances of the app
    Write-Output "Stopping any active instances of voice-changer..."
    try { Stop-Process -Name "voice-changer" -Force -ErrorAction Stop } catch {}
    Start-Sleep -Seconds 1

    # 3. Create install directories
    New-Item -ItemType Directory -Force -Path $installFolder | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $installFolder "sounds") | Out-Null

    # 4. Copy files from packaging directory to installation target
    Write-Output "Copying application files..."

    $filesToCopy = @("voice-changer.exe", "README.txt", "Uninstall.ps1", "Uninstall.bat")
    foreach ($name in $filesToCopy) {
        $src = Join-Path $scriptDir $name
        if (Test-Path $src) {
            Copy-Item -Force -Path $src -Destination $installFolder
        } else {
            Write-Warning "Missing file: $src - skipping."
        }
    }

    $soundsSource = Join-Path $scriptDir "sounds"
    if (Test-Path $soundsSource) {
        Copy-Item -Recurse -Force -Path (Join-Path $soundsSource "*") -Destination (Join-Path $installFolder "sounds")
    }

    # 5. Create Shortcuts (Desktop & Start Menu)
    Write-Output "Creating shortcuts..."
    $WshShell = New-Object -ComObject WScript.Shell

    if (!(Test-Path $programsPath)) {
        New-Item -ItemType Directory -Force -Path $programsPath | Out-Null
    }

    foreach ($lnkDir in @($desktopPath, $programsPath)) {
        $shortcut = $WshShell.CreateShortcut((Join-Path $lnkDir "$appDisplayName.lnk"))
        $shortcut.TargetPath = Join-Path $installFolder "voice-changer.exe"
        $shortcut.WorkingDirectory = $installFolder
        $shortcut.Description = "Real-time Voice Changer and Soundboard"
        $shortcut.Save()
    }

    # 6. Register in Windows Add/Remove Programs (HKCU for user-space list)
    Write-Output "Registering in Windows Installed Apps..."
    $registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appName"

    if (!(Test-Path $registryPath)) {
        New-Item -Path $registryPath -Force | Out-Null
    }

    $uninstallScriptPath = Join-Path $installFolder "Uninstall.ps1"
    $uninstallCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScriptPath`""

    New-ItemProperty -Path $registryPath -Name "DisplayName" -Value $appDisplayName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "UninstallString" -Value $uninstallCmd -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "InstallLocation" -Value $installFolder -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "DisplayVersion" -Value "1.1.0" -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "Publisher" -Value "Antigravity" -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "NoModify" -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name "NoRepair" -Value 1 -PropertyType DWord -Force | Out-Null

    Write-Output "=================================================="
    Write-Output "INSTALLATION COMPLETE!"
    Write-Output ""
    Write-Output "Launch '$appDisplayName' from your Desktop or Start Menu."
    Write-Output ""
    Write-Output "OUTPUT SETUP (how to be heard in Discord/games):"
    Write-Output " 1. Install the free VB-CABLE driver from vb-audio.com/Cable and reboot."
    Write-Output " 2. In the app: Mic Input = your microphone,"
    Write-Output "    Primary Output = 'CABLE Input (VB-Audio Virtual Cable)',"
    Write-Output "    Voice Monitor = your headphones."
    Write-Output " 3. Press START AUDIO ENGINE."
    Write-Output " 4. In Discord/your game, set the microphone to"
    Write-Output "    'CABLE Output (VB-Audio Virtual Cable)'."
    Write-Output "=================================================="
}
catch {
    Write-Error "Installation failed: $($_.Exception.Message)"
    throw
}
