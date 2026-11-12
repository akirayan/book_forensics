@echo off
:: MWUA -- Make Windows Unix-like Again
:: Install OpenSSH Server (sshd): auto-start service + inbound firewall rule on TCP 22.
:: ver1.1 by yan.akira@gmail.com
::
:: Usage: run as Administrator:  mwua_setup_ssh.cmd

:: Check for Administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Please run this script as Administrator.
    exit /b 1
)

set "PS_FILE=%TEMP%\install_ssh.ps1"

echo Write-Host "Installing OpenSSH via WinGet..." -ForegroundColor Cyan > "%PS_FILE%"
:: Use --silent. If it fails, WinGet usually falls back to interactive.
echo winget install --id Microsoft.OpenSSH.Preview -e --silent -s winget --accept-package-agreements --accept-source-agreements >> "%PS_FILE%"

:: Enable the service if it was created
echo if (Get-Service sshd -ErrorAction SilentlyContinue) { >> "%PS_FILE%"
echo     Set-Service -Name sshd -StartupType 'Automatic' >> "%PS_FILE%"
echo     if ((Get-Service sshd).Status -ne 'Running') { Start-Service sshd } >> "%PS_FILE%"
echo } >> "%PS_FILE%"

:: Firewall rule (essential for remote access)
echo if (!(Get-NetFirewallRule -Name "OpenSSH-Server-In-TCP" -ErrorAction SilentlyContinue)) { >> "%PS_FILE%"
echo     New-NetFirewallRule -Name "OpenSSH-Server-In-TCP" -DisplayName "OpenSSH Server (sshd)" -Enabled True -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow >> "%PS_FILE%"
echo } >> "%PS_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
del "%PS_FILE%"

echo [INFO] OpenSSH Server setup complete. Access: ssh %USERNAME%@this-host
exit /b 0
