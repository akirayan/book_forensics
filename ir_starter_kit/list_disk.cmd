@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM list_disk.cmd
REM   get info for: Disk / Volume / BitLocker / SecureBoot
REM
REM Usage:
REM   list_disk.cmd CASE_NO
REM ============================================================






@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 CASE_NO
    exit /b 1
)

set "CASE_NO=%~1"
set "KIT_ROOT=%~dp0"
set "CASE_ROOT=%KIT_ROOT%CASES\%CASE_NO%"
set "LOG_DIR=%CASE_ROOT%\logs"
set "LOGFILE=%LOG_DIR%\disk-info.log"

if not exist "%LOG_DIR%" (
    echo ERROR: Case directory not found.
    echo Run create_case_dirs.cmd %CASE_NO% first.
    exit /b 1
)

echo Writing: %LOGFILE%

(
echo =====================================
echo Disk Information
echo =====================================
echo Case=%CASE_NO%
echo Date=%DATE% %TIME%
echo.
) > "%LOGFILE%"

echo [DISKS]>> "%LOGFILE%"
echo.>> "%LOGFILE%"

powershell -NoProfile -Command "Get-Disk | Select-Object Number,FriendlyName,SerialNumber,HealthStatus,OperationalStatus,@{N='TotalSizeGB';E={[math]::Round($_.Size/1GB,2)}},PartitionStyle | Format-Table -AutoSize" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"
echo.>> "%LOGFILE%"

echo [PARTITIONS]>> "%LOGFILE%"
echo.>> "%LOGFILE%"

powershell -NoProfile -Command "Get-Disk | ForEach-Object { $_ | Get-Partition | Format-Table PartitionNumber,DriveLetter,Offset,Size,Type -AutoSize; '' }" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"
echo.>> "%LOGFILE%"

echo [VOLUMES]>> "%LOGFILE%"
echo.>> "%LOGFILE%"

powershell -NoProfile -Command "Get-Volume | Select-Object DriveLetter,FileSystemType,DriveType,HealthStatus,OperationalStatus,SizeRemaining,Size | Format-Table -AutoSize" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"
echo.>> "%LOGFILE%"

echo [BITLOCKER]>> "%LOGFILE%"
echo.>> "%LOGFILE%"

powershell -NoProfile -Command "try { Get-BitLockerVolume | Format-Table -AutoSize } catch { 'BitLocker cmdlets unavailable' }" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"

echo [SECUREBOOT]>> "%LOGFILE%"

powershell -NoProfile -Command "try { Confirm-SecureBootUEFI } catch { 'Unsupported or Legacy BIOS' }" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"

echo [MEMORY_INTEGRITY]>> "%LOGFILE%"

reg query "HKLM\System\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled >nul 2>&1

if errorlevel 1 (
    echo Not Configured>> "%LOGFILE%"
) else (
    for /f "tokens=3" %%A in ('reg query "HKLM\System\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled ^| find "Enabled"') do (
        if /I "%%A"=="0x1" (
            echo Enabled>> "%LOGFILE%"
        ) else (
            echo Disabled>> "%LOGFILE%"
        )
    )
)

echo.>> "%LOGFILE%"

echo [VBS]>> "%LOGFILE%"

powershell -NoProfile -Command "try { $v=(Get-CimInstance -ClassName Win32_DeviceGuard -ErrorAction Stop).VirtualizationBasedSecurityStatus; switch ($v) { 0 {'Disabled'} 1 {'Enabled_Not_Running'} 2 {'Running'} default {$v} } } catch { 'Not Available' }" >> "%LOGFILE%" 2>&1

echo.>> "%LOGFILE%"

echo Done.
echo Log file:
echo %LOGFILE%

endlocal
exit /b 0
