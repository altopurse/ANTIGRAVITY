# Antigravity Voice Engine - Native Windows Installer (User-space)
# Runs without requiring administrator privileges!

$ErrorActionPreference = "Stop"

# 1. Setup paths
$appName = "AntigravityVoiceEngine"
$appDisplayName = "Antigravity Voice Engine"
$installFolder = Join-Path $env:LOCALAPPDATA $appName
$desktopPath = [System.Environment]::GetFolderPath("Desktop")
$startMenuPath = [System.Environment]::GetFolderPath("StartMenu")
$programsPath = Join-Path $startMenuPath "Programs"

Write-Output "=================================================="
Write-Output "Installing $appDisplayName..."
Write-Output "Target: $installFolder"
Write-Output "=================================================="

# 2. Stop running instances of the app
Write-Output "Stopping any active instances of voice changer..."
Stop-Process -Name "voice-changer" -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 3. Create install directories
New-Item -ItemType Directory -Force -Path $installFolder | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $installFolder "sounds") | Out-Null

# 4. Copy files from packaging directory to installation target
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Write-Output "Copying application binaries..."

Copy-Item -Force (Join-Path $scriptDir "voice-changer.exe") -Destination $installFolder
Copy-Item -Force (Join-Path $scriptDir "README.txt") -Destination $installFolder
Copy-Item -Force (Join-Path $scriptDir "Uninstall.ps1") -Destination $installFolder
Copy-Item -Recurse -Force (Join-Path $scriptDir "sounds\*") -Destination (Join-Path $installFolder "sounds")

# 5. Create Shortcuts (Desktop & Start Menu)
Write-Output "Creating system shortcuts..."
$WshShell = New-Object -ComObject WScript.Shell

# Desktop Shortcut
$desktopShortcut = $WshShell.CreateShortcut((Join-Path $desktopPath "$appDisplayName.lnk"))
$desktopShortcut.TargetPath = Join-Path $installFolder "voice-changer.exe"
$desktopShortcut.WorkingDirectory = $installFolder
$desktopShortcut.Description = "Real-time Voice Changer and Soundboard"
$desktopShortcut.Save()

# Start Menu Shortcut
$startMenuShortcut = $WshShell.CreateShortcut((Join-Path $programsPath "$appDisplayName.lnk"))
$startMenuShortcut.TargetPath = Join-Path $installFolder "voice-changer.exe"
$startMenuShortcut.WorkingDirectory = $installFolder
$startMenuShortcut.Description = "Real-time Voice Changer and Soundboard"
$startMenuShortcut.Save()

# 6. Register in Windows Add/Remove Programs (HKCU for user-space list)
Write-Output "Registering app in Windows Installed Apps registry..."
$registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appName"

if (!(Test-Path $registryPath)) {
    New-Item -Path $registryPath -Force | Out-Null
}

$uninstallCmd = "powershell.exe -ExecutionPolicy Bypass -File `"`"$installFolder\Uninstall.ps1`"`""

New-ItemProperty -Path $registryPath -Name "DisplayName" -Value $appDisplayName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "UninstallString" -Value $uninstallCmd -PropertyType String -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "InstallLocation" -Value $installFolder -PropertyType String -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "DisplayVersion" -Value "1.0.0" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "Publisher" -Value "DeepMind Antigravity Team" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "NoModify" -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $registryPath -Name "NoRepair" -Value 1 -PropertyType DWord -Force | Out-Null

Write-Output "=================================================="
Write-Output "INSTALLATION COMPLETE!"
Write-Output "You can launch '$appDisplayName' from your Desktop or Start Menu."
Write-Output "=================================================="
