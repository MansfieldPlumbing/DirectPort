<#
.SYNOPSIS
    Shell Extension Registration Utility
#>
$ErrorActionPreference = 'Stop'
$dllFile = "menudump.dll"

if (-not (Test-Path $dllFile)) {
    Write-Host " [FAIL] $dllFile not found. Compile the project first." -ForegroundColor Red
    exit 1
}

Write-Host " [WARN] Terminating Shell to release COM locks..." -ForegroundColor Yellow
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2 

Write-Host " [INFO] Unregistering previous DLL state..." -ForegroundColor Yellow
$unregProc = Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s", "/u", $dllFile -Wait -PassThru -NoNewWindow

Write-Host " [INFO] Registering new DLL..." -ForegroundColor Cyan
$regProc = Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s", $dllFile -Wait -PassThru -NoNewWindow

if ($regProc.ExitCode -ne 0) {
    Write-Host " [FAIL] DLL Registration failed with exit code $($regProc.ExitCode)." -ForegroundColor Red
} else {
    Write-Host " [INFO] DLL successfully registered to the system." -ForegroundColor Green
}

Write-Host " [INFO] Starting Shell with new extension..." -ForegroundColor Green
Start-Process explorer.exe