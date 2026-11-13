@echo off
setlocal EnableExtensions

REM ============================================================
REM dump_memory.cmd
REM Memory acquisition using DumpIt
REM
REM Usage:
REM   dump_memory.cmd CASE_NO
REM ============================================================

if "%~2"=="" (
    echo Usage:
    echo   dump_memory.cmd CASE_NO EVIDENCE_ID
    echo   EVIDENCE_ID: evidence id for a PC/server/HDD, e.g. E001
    exit /b 1
)

REM ============================================================
REM Require Administrator
REM ============================================================

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo ERROR: Administrator privileges required.
    exit /b 1
)

set "CASE_NO=%~1"
set "EVIDENCE_ID=%~2"

REM Remove trailing backslash from script path

set "KIT_ROOT=%~dp0"
if "%KIT_ROOT:~-1%"=="\" set "KIT_ROOT=%KIT_ROOT:~0,-1%"

REM CASES under the CURRENT directory (cd to your save media first). DumpIt stays with the kit (KIT_ROOT).
set "CASE_DIR=%CD%\CASES\%CASE_NO%\%EVIDENCE_ID%"
set "MEM_DIR=%CASE_DIR%\memory"
set "LOG_DIR=%CASE_DIR%\logs"

if not exist "%CASE_DIR%" (
    echo ERROR: Case directory not found:
    echo %CASE_DIR%
    exit /b 1
)

if not exist "%MEM_DIR%" mkdir "%MEM_DIR%"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

set "LOGFILE=%LOG_DIR%\memory_acquisition.log"

echo ===================================================== > "%LOGFILE%"
echo Memory Acquisition Log >> "%LOGFILE%"
echo ===================================================== >> "%LOGFILE%"
echo Case Number: %CASE_NO% >> "%LOGFILE%"
echo Evidence ID: %EVIDENCE_ID% >> "%LOGFILE%"
echo Start Time : %date% %time% >> "%LOGFILE%"

REM ============================================================
REM Detect OS Architecture
REM ============================================================

for /f %%A in ('
powershell -NoProfile -Command "[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture"
') do set "OSARCH=%%A"

echo Architecture: %OSARCH%
echo Architecture: %OSARCH% >> "%LOGFILE%"

if /I "%OSARCH%"=="Arm64" (
    set "DUMPIT=%KIT_ROOT%\tools\dumpit\ARM64\DumpIt.exe"
) else if /I "%OSARCH%"=="X64" (
    set "DUMPIT=%KIT_ROOT%\tools\dumpit\x64\DumpIt.exe"
) else (
    set "DUMPIT=%KIT_ROOT%\tools\dumpit\x86\DumpIt.exe"
)

echo DumpIt: %DUMPIT%
echo DumpIt: %DUMPIT% >> "%LOGFILE%"

if not exist "%DUMPIT%" (
    echo ERROR: DumpIt executable not found.
    echo %DUMPIT%

    echo ERROR: DumpIt executable not found. >> "%LOGFILE%"
    exit /b 1
)

REM ============================================================
REM Move to memory output directory
REM ============================================================

pushd "%MEM_DIR%"

echo.
echo =========================================
echo Memory Acquisition
echo =========================================
echo.
echo Case Number : %CASE_NO%
echo Architecture: %OSARCH%
echo Output Dir  : %MEM_DIR%
echo.

echo Launching DumpIt...
echo.

"%DUMPIT%" /Q >> "%LOGFILE%" 2>&1

set "RC=%ERRORLEVEL%"

popd

echo. >> "%LOGFILE%"
echo End Time: %date% %time% >> "%LOGFILE%"
echo Exit Code: %RC% >> "%LOGFILE%"

if not "%RC%"=="0" (
    echo.
    echo FAILED
    echo.
    echo See log:
    echo   %LOGFILE%
    exit /b %RC%
)

echo.
echo SUCCESS
echo.
echo Memory dump files:
echo.

dir /b "%MEM_DIR%\*.dmp" 2>nul

echo.
echo Log:
echo   %LOGFILE%

exit /b 0
