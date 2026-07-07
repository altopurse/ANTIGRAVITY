# Antigravity Voice Engine - Native Windows Uninstaller (User-space)
# Runs without requiring administrator privileges!

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
Stop-Process -Name "voice-changer" -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 2. Remove shortcuts
Write-Output "Removing system shortcuts..."
$desktopLnk = Join-Path $desktopPath "$appDisplayName.lnk"
if (Test-Path $desktopLnk) {
    Remove-Item -Force $desktopLnk
}

$startMenuLnk = Join-Path $programsPath "$appDisplayName.lnk"
if (Test-Path $startMenuLnk) {
    Remove-Item -Force $startMenuLnk
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
    # Delete everything in the install folder except the uninstaller first
    Get-ChildItem -Path $installFolder -Exclude "Uninstall.ps1" | Remove-Item -Recurse -Force
    
    # Self-deletion trick: Start a background command to wait 1 second and then remove the uninstaller script and directory
    $parentPath = Split-Path -Path $installFolder -Parent
    $folderName = Split-Path -Path $installFolder -Leaf
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c timeout /t 1 && rmdir /s /q `"$installFolder`"" -WindowStyle Hidden
}

Write-Output "=================================================="
Write-Output "UNINSTALLATION COMPLETE!"
Write-Output "=================================================="
