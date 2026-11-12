@echo off
setlocal

chcp 65001 >nul

if "%~1"=="" (
    echo Usage: get_system_info.cmd CASE_NO
    exit /b 1
)

set CASE_NO=%~1
set KIT_ROOT=%~dp0

REM CASES is under the CURRENT directory; create_case_dirs.cmd (sibling) inherits this cwd.
call "%KIT_ROOT%create_case_dirs.cmd" %CASE_NO% >nul

set LOG=%CD%\CASES\%CASE_NO%\logs\system-info.log

echo Writing: %LOG%

(
echo =====================================
echo System Information
echo =====================================
echo Case=%CASE_NO%
echo Date=%DATE% %TIME%
echo.

echo [HOSTNAME]
hostname

echo.
echo [WHOAMI]
whoami

echo.
echo [DATE]
powershell -NoProfile -Command "Get-Date -Format o"

echo.
echo [SYSTEMINFO]
systeminfo

echo.
echo [IPCONFIG]
ipconfig /all

echo.
echo [ROUTES]
route print

echo.
echo [TIMEZONE]
tzutil /g

echo.
echo [DEVICE GUARD]
powershell -NoProfile -Command "Get-CimInstance Win32_DeviceGuard | Format-List *"

echo.
echo [BCD]
bcdedit

echo.
echo [WHOAMI ALL]
whoami /all

) > "%LOG%" 2>&1

echo Done.
echo %LOG%
