@echo off
setlocal

REM ============================================================
REM ewf_report.cmd
REM   Build a Markdown acquisition report from the logs under
REM   CASES\<CASE_NO>\  (counterpart of ewf_report.sh).
REM
REM Usage:
REM   ewf_report.cmd CASE_NO EVIDENCE_ID TARGET
REM     EVIDENCE_ID: evidence id for a PC/server/HDD, e.g. E001
REM     TARGET: the log prefix used at acquisition time (disk0 / driveC ...)
REM
REM Examples:
REM   ewf_report.cmd C26011 E001 disk0
REM   ewf_report.cmd C26012 E001 driveC
REM
REM CASES is read from the CURRENT directory; cd to your save media first.
REM ============================================================

if "%~3"=="" (
    echo Usage:
    echo   %~nx0 CASE_NO EVIDENCE_ID TARGET
    echo.
    echo Examples:
    echo   %~nx0 C26011 E001 disk0
    echo   %~nx0 C26012 E001 driveC
    exit /b 1
)

set "CASE_NO=%~1"
set "EVIDENCE_ID=%~2"
set "TARGET=%~3"

set "KIT_ROOT=%~dp0"
set "CASE_ROOT=%CD%\CASES\%CASE_NO%\%EVIDENCE_ID%"

set "LOG_DIR=%CASE_ROOT%\logs"
set "REPORT_DIR=%CASE_ROOT%\reports"

set "DISKINFO=%LOG_DIR%\disk-info.log"
set "SYSINFO=%LOG_DIR%\system-info.log"
set "ACQLOG=%LOG_DIR%\%TARGET%_acquisition.log"
set "VERIFYLOG=%LOG_DIR%\%TARGET%_verify.log"
set "INFOLOG=%LOG_DIR%\%TARGET%_info.log"

set "REPORT=%REPORT_DIR%\acquisition_report.md"

if not exist "%REPORT_DIR%" mkdir "%REPORT_DIR%"

echo Writing: %REPORT%

(
echo # Acquisition Report
echo.
echo ## Case Information
echo.
echo ^| Item ^| Value ^|
echo ^|------^|------^|
echo ^| Case Number ^| %CASE_NO% ^|
echo ^| Evidence ID ^| %EVIDENCE_ID% ^|
echo ^| Target ^| %TARGET% ^|
echo.
echo ---
echo.
echo ## Available Logs
echo.
echo ```text
if exist "%DISKINFO%" echo disk-info.log
if exist "%SYSINFO%" echo system-info.log
if exist "%ACQLOG%" echo %TARGET%_acquisition.log
if exist "%VERIFYLOG%" echo %TARGET%_verify.log
if exist "%INFOLOG%" echo %TARGET%_info.log
echo ```
echo.
echo ---
echo.
echo ## Security Configuration
echo.
) > "%REPORT%"

findstr /C:"[BITLOCKER]" /C:"[SECUREBOOT]" /C:"[MEMORY_INTEGRITY]" /C:"[VBS]" "%DISKINFO%" >> "%REPORT%" 2>nul

(
echo.
echo ---
echo.
echo ## Acquisition Log Excerpts
echo.
echo ```text
) >> "%REPORT%"

powershell -NoProfile -Command "Get-Content '%ACQLOG%' -Tail 30" >> "%REPORT%" 2>nul

(
echo ```
echo.
echo ---
echo.
echo ## Verification Log Excerpts
echo.
echo ```text
) >> "%REPORT%"

if exist "%VERIFYLOG%" (
    powershell -NoProfile -Command "Get-Content '%VERIFYLOG%' -Tail 30" >> "%REPORT%"
)

(
echo ```
echo.
echo ---
echo.
echo ## EWF Metadata
echo.
echo ```text
) >> "%REPORT%"

if exist "%INFOLOG%" (
    powershell -NoProfile -Command "Get-Content '%INFOLOG%' -Tail 100" >> "%REPORT%"
)

(
echo ```
echo.
echo ---
echo.
echo ## Evidence Files
echo.
echo ```text
) >> "%REPORT%"

dir /b "%CASE_ROOT%\acquisition" >> "%REPORT%" 2>nul

(
echo ```
echo.
echo ---
echo.
echo ## Examiner Notes
echo.
echo - Acquisition completed.
echo - Review logs for details.
echo - Verify hash values before evidence submission.
echo.
) >> "%REPORT%"

echo Done.
echo %REPORT%

endlocal
exit /b 0
