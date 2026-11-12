@echo off
setlocal

REM ============================================================
REM ewf_imager.cmd
REM
REM Usage:
REM   ewf_imager.cmd CASE_NO DESCRIPTION EXAMINER TARGET
REM
REM   TARGET:
REM     Number N -> \\.\PhysicalDriveN  (physical disk; BitLocker = ciphertext)
REM     Letter C -> \\.\C:             (logical drive; live OS decrypts = plaintext; no key needed)
REM
REM Example:
REM   ewf_imager.cmd C26010 "Live Windows11 Arm64, disk0" endoaki 0
REM   ewf_imager.cmd C26011 "Logical Drive C"             endoaki C
REM ============================================================

if "%~4"=="" (
    echo Usage:
    echo %~nx0 CASE_NO DESCRIPTION EXAMINER TARGET
    echo   TARGET: 0..N = PhysicalDriveN / C,D.. = logical drive
    exit /b 1
)

set "CASE_NO=%~1"
set "DESCRIPTION=%~2"
set "EXAMINER=%~3"
set "TARGET=%~4"

set "KIT_ROOT=%~dp0"

set "CASE_ROOT=%KIT_ROOT%CASES\%CASE_NO%"
set "ACQ_DIR=%CASE_ROOT%\acquisition"
set "LOG_DIR=%CASE_ROOT%\logs"

set "EWFACQUIRE=%KIT_ROOT%tools\ewftools-x64\ewfacquire.exe"
set "EWFVERIFY=%KIT_ROOT%tools\ewftools-x64\ewfverify.exe"
set "EWFINFO=%KIT_ROOT%tools\ewftools-x64\ewfinfo.exe"

if not exist "%ACQ_DIR%" (
    echo ERROR: Case directory not found.
    echo Run create_case_dirs.cmd %CASE_NO% first.
    exit /b 1
)

if not exist "%EWFACQUIRE%" (
    echo ERROR: %EWFACQUIRE% not found.
    exit /b 1
)

if not exist "%EWFVERIFY%" (
    echo ERROR: %EWFVERIFY% not found.
    exit /b 1
)

if not exist "%EWFINFO%" (
    echo ERROR: %EWFINFO% not found.
    exit /b 1
)

REM ------------------------------------------------------------
REM TARGET detection
REM   Number 0,1,.. -> \\.\PhysicalDriveN  (physical; BitLocker = ciphertext)
REM   Letter C,D,.. -> \\.\C:             (logical; live OS decrypts = plaintext)
REM NOTE: no space before the pipe to findstr (trailing space breaks the regex)
REM ------------------------------------------------------------
echo %TARGET%| findstr /r "^[0-9][0-9]*$" >nul
if not errorlevel 1 (
    set "SOURCE=\\.\PhysicalDrive%TARGET%"
    set "IMAGE_BASE=%CASE_NO%_disk%TARGET%"
    set "ACQ_LOG=%LOG_DIR%\disk%TARGET%_acquisition.log"
    set "VERIFY_LOG=%LOG_DIR%\disk%TARGET%_verify.log"
    set "INFO_LOG=%LOG_DIR%\disk%TARGET%_info.log"
    goto target_ok
)

echo %TARGET%| findstr /r "^[A-Za-z]$" >nul
if not errorlevel 1 (
    set "SOURCE=\\.\%TARGET%:"
    set "IMAGE_BASE=%CASE_NO%_drive%TARGET%"
    set "ACQ_LOG=%LOG_DIR%\drive%TARGET%_acquisition.log"
    set "VERIFY_LOG=%LOG_DIR%\drive%TARGET%_verify.log"
    set "INFO_LOG=%LOG_DIR%\drive%TARGET%_info.log"
    goto target_ok
)

echo ERROR: Invalid target [%TARGET%]
echo   examples:  %~nx0 C26011 "Physical Disk 0" endoaki 0
echo              %~nx0 C26011 "Logical Drive C" endoaki C
exit /b 1

:target_ok
set "E01_FILE=%ACQ_DIR%\%IMAGE_BASE%.E01"

echo Source     : %SOURCE%
echo Image Base : %IMAGE_BASE%
echo.

(
echo ===========================================================
echo Acquisition Log
echo ===========================================================
echo Case Number : %CASE_NO%
echo Examiner    : %EXAMINER%
echo Description : %DESCRIPTION%
echo Source Disk : %SOURCE%
echo Output File : %E01_FILE%
echo Start Time  : %DATE% %TIME%
echo ===========================================================
echo.
) > "%ACQ_LOG%"

echo.
echo ===========================================================
echo Starting acquisition...
echo Case       : %CASE_NO%
echo Target     : %TARGET%
echo Examiner   : %EXAMINER%
echo ===========================================================
echo.

REM ewfacquire: -d sha1 (secondary digest), -C/-D/-e (case/description/examiner),
REM             -S 4G (segment split), -g 64 (skip on read error).
REM             Same flags as ewf_imager.sh (earlier the description was wrongly passed to -d).
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%EWFACQUIRE%' -u -t '%ACQ_DIR%\%IMAGE_BASE%' -c fast -f encase6 -S 4G -d sha1 -g 64 -C '%CASE_NO%' -D '%DESCRIPTION%' -e '%EXAMINER%' '%SOURCE%' 2>&1 | Tee-Object -FilePath '%ACQ_LOG%' -Append"

set "RC=%ERRORLEVEL%"

echo.>> "%ACQ_LOG%"
echo Finish Time : %DATE% %TIME%>> "%ACQ_LOG%"
echo Return Code : %RC%>> "%ACQ_LOG%"

if not "%RC%"=="0" (
    echo.
    echo ERROR: Acquisition failed.
    exit /b %RC%
)

echo.
echo ===========================================================
echo Acquisition completed successfully.
echo ===========================================================
echo.

(
echo ===========================================================
echo Verify Log
echo ===========================================================
echo Case Number : %CASE_NO%
echo Target      : %TARGET%
echo Image File  : %E01_FILE%
echo Start Time  : %DATE% %TIME%
echo ===========================================================
echo.
) > "%VERIFY_LOG%"

echo Starting verification...
echo.


"%EWFVERIFY%" "%E01_FILE%" >> "%VERIFY_LOG%" 2>&1


set "VERIFY_RC=%ERRORLEVEL%"

echo.>> "%VERIFY_LOG%"
echo Finish Time : %DATE% %TIME%>> "%VERIFY_LOG%"
echo Return Code : %VERIFY_RC%>> "%VERIFY_LOG%"

echo.
echo ===========================================================
if "%VERIFY_RC%"=="0" (
    echo Verification completed successfully.
) else (
    echo Verification FAILED.
)
echo ===========================================================
echo.

rem echo Acquisition Log:
rem echo %ACQ_LOG%

rem echo.

rem echo Verify Log:
rem echo %VERIFY_LOG%

rem echo.



(
echo ===========================================================
echo EWF Info Log
echo ===========================================================
echo Case Number : %CASE_NO%
echo Target      : %TARGET%
echo Image File  : %E01_FILE%
echo Start Time  : %DATE% %TIME%
echo ===========================================================
echo.
) > "%INFO_LOG%"

echo Starting ewfinfo...
echo.

"%EWFINFO%" "%E01_FILE%" >> "%INFO_LOG%" 2>&1

echo.>> "%INFO_LOG%"
echo Finish Time : %DATE% %TIME%>> "%INFO_LOG%"

REM ------------------------------------------------------------
REM FTK Imager style summary: <image>.E01.txt next to the E01 (same as ewf_imager.sh)
REM ------------------------------------------------------------
set "E01_TXT=%ACQ_DIR%\%IMAGE_BASE%.E01.txt"
(
echo Created By ewf_imager.cmd ^(libewf / ewftools^)
echo.
echo Case Information:
echo   Case Number:         %CASE_NO%
echo   Unique Description:  %DESCRIPTION%
echo   Examiner:            %EXAMINER%
echo.
echo --------------------------------------------------------------
echo Information for %E01_FILE%:
echo.
echo   Source:              %SOURCE%
echo   Image Type:          EnCase6 ^(E01^), compression=fast, segment=4GiB
echo   Times:               see logs\ acquisition/verify logs
echo.
echo --------------------------------------------------------------
echo Image Verification Results:
findstr /c:"hash stored" /c:"hash calculated" /c:"ewfverify:" "%VERIFY_LOG%"
) > "%E01_TXT%"

echo.
echo Summary : %E01_TXT%

exit /b %VERIFY_RC%

