@echo off
setlocal

if "%~1"=="" (
    echo Usage: create_case_dirs.cmd CASE_NO
    exit /b 1
)

set CASE_NO=%~1
set KIT_ROOT=%~dp0

set CASE_ROOT=%KIT_ROOT%CASES\%CASE_NO%
set LOG_DIR=%CASE_ROOT%\logs
set ACQ_DIR=%CASE_ROOT%\acquisition
set REPORT_DIR=%CASE_ROOT%\reports

if not exist "%KIT_ROOT%CASES" mkdir "%KIT_ROOT%CASES"

if not exist "%CASE_ROOT%" mkdir "%CASE_ROOT%"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
if not exist "%ACQ_DIR%" mkdir "%ACQ_DIR%"
if not exist "%REPORT_DIR%" mkdir "%REPORT_DIR%"

echo CASE_ROOT=%CASE_ROOT%
echo LOG_DIR=%LOG_DIR%
echo ACQ_DIR=%ACQ_DIR%
echo REPORT_DIR=%REPORT_DIR%

exit /b 0

