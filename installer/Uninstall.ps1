# Antigravity Voice Engine - Uninstaller (user-space, no admin required)
# Run via Uninstall.bat, or:  powershell -ExecutionPolicy Bypass -File Uninstall.ps1

$ErrorActionPreference = "Continue" # Continue on non-critical removal warnings

$appName = "AntigravityVoiceEngine"
$appDisplayName = "Antigravity Voice Engine"
$installFolder = Join-Path $env:LOCALAPPDATA $appName
$desktopPath = [System.Environment]::GetFolderPath("Desktop")
$startMenuPath = [System.Environment]::GetFolderPath("StartMenu")
$programsPath = Join-Path $startMenuPath "Programs"

Write-Output "=================================================="
Write-Output "Uninstalling $appDisplayName..."
Write-Output "=================================================="

# 1. Stop active process
Write-Output "Stopping active processes..."
try { Stop-Process -Name "voice-changer" -Force -ErrorAction Stop } catch {}
Start-Sleep -Seconds 1

# 2. Remove shortcuts
Write-Output "Removing shortcuts..."
foreach ($lnkDir in @($desktopPath, $programsPath)) {
    $lnk = Join-Path $lnkDir "$appDisplayName.lnk"
    if (Test-Path $lnk) {
        Remove-Item -Force $lnk
    }
}

# 3. Remove Registry Entry
Write-Output "Cleaning Windows App Registry..."
$registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appName"
if (Test-Path $registryPath) {
    Remove-Item -Path $registryPath -Recurse -Force
}

# 4. Remove Files
Write-Output "Removing files from $installFolder..."
if (Test-Path $installFolder) {
    # Delete everything except the uninstaller scripts first (they are running)
    Get-ChildItem -Path $installFolder -Exclude "Uninstall.ps1", "Uninstall.bat" | Remove-Item -Recurse -Force

    # Self-deletion: background command waits 1 second, then removes the folder
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c timeout /t 1 && rmdir /s /q `"$installFolder`"" -WindowStyle Hidden
}

Write-Output "=================================================="
Write-Output "UNINSTALLATION COMPLETE!"
Write-Output "=================================================="
