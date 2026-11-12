@echo off
:: MWUA -- Make Windows Unix-like Again
:: Set the hostname and a fixed (static) IPv4 address / gateway / DNS.
:: ver1.1 by yan.akira@gmail.com
::
:: Usage (run as Administrator):
::    mwua_setup_ip.cmd HOSTNAME IPADDRESS GATEWAY
::    eg. mwua_setup_ip.cmd WIN11JA111 172.16.28.131 172.16.28.2

:: 1. Arguments
if "%~3"=="" (
    echo [USAGE] %0 HOSTNAME IPADDRESS GATEWAY
    echo     eg. %0 WIN11JA111 172.16.28.131 172.16.28.2
    exit /b 1
)
set "NEW_HOSTNAME=%~1"
set "NEW_IP=%~2"
set "NEW_GW=%~3"

:: 2. Administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Please run this script as Administrator.
    exit /b 1
)

:: --- Set hostname (takes effect after reboot) ---
set "PS_FILE=%TEMP%\set_hostname.ps1"
echo Write-Host "Changing Hostname to %NEW_HOSTNAME%..." -ForegroundColor Cyan > "%PS_FILE%"
echo Rename-Computer -NewName "%NEW_HOSTNAME%" -Force -ErrorAction SilentlyContinue >> "%PS_FILE%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
del "%PS_FILE%"

:: --- Set static IP / gateway / DNS on the first "Up" adapter ---
set "PS_FILE=%TEMP%\set_ipaddress.ps1"
echo $Adapter = (Get-NetAdapter ^| Where-Object Status -eq 'Up' ^| Select-Object -First 1) > "%PS_FILE%"
echo Write-Host "Force-resetting IP stack on $($Adapter.Name)..." -ForegroundColor Yellow >> "%PS_FILE%"
:: Wipe the interface config first (reliable for gateway changes)
echo netsh interface ip set address name="$($Adapter.Name)" source=dhcp >> "%PS_FILE%"
echo netsh interface ip delete dns name="$($Adapter.Name)" all >> "%PS_FILE%"
echo Start-Sleep -Seconds 2 >> "%PS_FILE%"
echo Write-Host "Configuring network to %NEW_IP%..." -ForegroundColor Cyan >> "%PS_FILE%"
echo New-NetIPAddress -InterfaceIndex $Adapter.InterfaceIndex -IPAddress "%NEW_IP%" -PrefixLength 24 -DefaultGateway "%NEW_GW%" -ErrorAction Stop >> "%PS_FILE%"
echo Set-DnsClientServerAddress -InterfaceIndex $Adapter.InterfaceIndex -ServerAddresses ("%NEW_GW%", "8.8.8.8") >> "%PS_FILE%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
del "%PS_FILE%"

echo.
echo    --- MWUA: hostname / fixed IP set ---
echo    HOSTNAME:  %NEW_HOSTNAME%
echo    IPADDRESS: %NEW_IP%/24   GATEWAY: %NEW_GW%
echo    (the hostname change needs a reboot to take effect)
echo.
exit /b 0
