@echo off
setlocal

REM ============================================================
REM create_case_dirs.cmd  (Windows counterpart of create_case_dirs.sh)
REM
REM One case (CASE_NO) can hold several PCs/servers/HDDs, so data is
REM stored per evidence item under an EVIDENCE_ID folder:
REM   CASES\<CASE_NO>\<EVIDENCE_ID>\{acquisition,memory,symbols,logs,reports}
REM
REM CASES is created under the CURRENT directory. cd to your save media
REM (e.g. the USB SSD) before running this from there.
REM
REM Usage:  create_case_dirs.cmd CASE_NO EVIDENCE_ID
REM   example:  create_case_dirs.cmd C26015 E001
REM ============================================================

if "%~2"=="" (
    echo Usage: create_case_dirs.cmd CASE_NO EVIDENCE_ID
    echo   EVIDENCE_ID: evidence id for a PC/server/HDD, e.g. E001
    exit /b 1
)

set "CASE_NO=%~1"
set "EVIDENCE_ID=%~2"
set "KIT_ROOT=%~dp0"

if /I "%CD%"=="%SystemRoot%\System32" (
    echo ERROR: You are in the system folder. CASES would be created here.
    echo Change to your save media first, then run from there.
    echo   example:  E:
    echo             cd \
    exit /b 1
)

set "CASE_ROOT=%CD%\CASES\%CASE_NO%\%EVIDENCE_ID%"
set "LOG_DIR=%CASE_ROOT%\logs"
set "ACQ_DIR=%CASE_ROOT%\acquisition"
set "MEM_DIR=%CASE_ROOT%\memory"
set "SYM_DIR=%CASE_ROOT%\symbols"
set "REPORT_DIR=%CASE_ROOT%\reports"

REM Create each level parent-first. On some shared-folder / network mounts,
REM mkdir does NOT auto-create intermediate directories (it fails with
REM "The system cannot find the file specified"), so we never rely on that.
if not exist "%CD%\CASES" mkdir "%CD%\CASES"
if not exist "%CD%\CASES\%CASE_NO%" mkdir "%CD%\CASES\%CASE_NO%"
if not exist "%CASE_ROOT%" mkdir "%CASE_ROOT%"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
if not exist "%ACQ_DIR%" mkdir "%ACQ_DIR%"
if not exist "%MEM_DIR%" mkdir "%MEM_DIR%"
if not exist "%SYM_DIR%" mkdir "%SYM_DIR%"
if not exist "%REPORT_DIR%" mkdir "%REPORT_DIR%"

echo CASE_ROOT=%CASE_ROOT%
echo LOG_DIR=%LOG_DIR%
echo ACQ_DIR=%ACQ_DIR%
echo MEM_DIR=%MEM_DIR%
echo SYM_DIR=%SYM_DIR%
echo REPORT_DIR=%REPORT_DIR%

exit /b 0
