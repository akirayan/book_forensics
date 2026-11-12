@echo off
:: MWUA -- Make Windows Unix-like Again
:: Install Git Bash, GCC, jq and the mount utility
:: to make the Windows as Unix/Linux
:: ver1.0 by yan.akira@gmail.com on 2026.1.12
::


:: 1. Check for Arguments

:: 2. Check for Administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Please run this script as Administrator.
    exit /b 1
)

:: 3. Detect CPU architecture (for display / future branching).
::    winget auto-selects arch for Git/jq. GCC(WinLibs) stays x64
::    and runs under x64 emulation on ARM64 (generated code is x64).
set "ARCH=%PROCESSOR_ARCHITECTURE%"
if /i "%ARCH%"=="AMD64" set "ARCH=x64"
echo [INFO] CPU architecture: %ARCH%

:: 4. All-users location for the generated commands (gitbash.cmd / mount_fileserver.cmd).
::    C:\Windows is always on PATH for every user, so no PATH edit is needed.
::    To use a dedicated folder instead, change ALLBIN and add it to the system PATH.
set "ALLBIN=C:\Windows"

::--------------------------- Main Code -------------------
call :Install_Git_Bash
call :Install_GCC
call :Install_Mount_Batch

:: or any other thing to add here

:: COMPLETED without error.
exit /b 0



::===================== subroutines ========================





:: Install Git-Bash and others
:Install_Git_Bash
  setlocal
    :: Powershell script
    set "PS_FILE=%TEMP%\install_git_bash.ps1"
    echo Write-Host "Installing Git Bash via WinGet..." -ForegroundColor Cyan > "%PS_FILE%"
    echo winget install --id Git.Git -e --silent -s winget --accept-package-agreements --accept-source-agreements >> "%PS_FILE%"
    :: add others, such as jq
    echo winget install --id jqlang.jq -e --silent -s winget --accept-package-agreements --accept-source-agreements >> "%PS_FILE%"

    :: Execute PowerShell and cleanup
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
    del "%PS_FILE%"

    :: using color prompt in Bash (add only once)
    set "BASHRC=%USERPROFILE%\.bashrc"
    findstr /c:"export PS1=" "%BASHRC%" >nul 2>&1 || echo export PS1='\[\e[32m\]\u@\h\[\e[m\] \[\e[34m\]\W\[\e[m\] \$ ' >> "%BASHRC%"

    :: link /mnt to /C/mnt, so we can use /mnt/fileserver/scripts/ as same as in Linux host
    set "FSTAB=C:\Program Files\Git\etc\fstab"
    set "MNT=C:\Program Files\Git\mnt"
    set "CMNT=C:\mnt"
    if not exist "%MNT%" ( mkdir "%MNT%")
    if not exist "%CMNT%" ( mkdir "%CMNT%")
    findstr /c:"C:/mnt" "%FSTAB%" >nul 2>&1 || (
        echo # link /mnt to /C/mnt, so we can use /mnt/fileserver/scripts/ as same as in Linux host >> "%FSTAB%"
        echo C:/mnt  /mnt  ntfs  binary,noacl,posix=0,user  0  0 >> "%FSTAB%"
    )

    :: create a simple batch to start Git-Bash (all-users location)
    if not exist "%ALLBIN%" ( mkdir "%ALLBIN%" )
    echo @echo off > "%ALLBIN%\gitbash.cmd"
    echo "c:\Program Files\git\bin\bash.exe" --login -i >>"%ALLBIN%\gitbash.cmd"

  endlocal
exit /b 0



:: Install GCC and related software
:Install_GCC
  setlocal
    if /i "%ARCH%"=="ARM64" echo [INFO] ARM64 host: installing x64 WinLibs GCC (runs under x64 emulation).
    set "PS_FILE=%TEMP%\install_gcc.ps1"

    echo Write-Host "Installing GCC (WinLibs UCRT)..." -ForegroundColor Cyan > "%PS_FILE%"
    :: WinLibs provides a solid UCRT-based GCC
    echo winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e --silent -s winget --accept-package-agreements --accept-source-agreements >> "%PS_FILE%"

    echo Write-Host "Configuring PATH for GCC..." -ForegroundColor Cyan >> "%PS_FILE%"
    :: Dynamic search handles the versioned install folder automatically
    echo $gccPath = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages", "C:\Program Files\WinLibs" -Filter "bin" -Recurse -ErrorAction SilentlyContinue ^| Select-Object -ExpandProperty FullName -First 1 >> "%PS_FILE%"
    echo if ($gccPath) { >> "%PS_FILE%"
    echo     $oldPath = [Environment]::GetEnvironmentVariable('Path', 'User'); >> "%PS_FILE%"
    echo     if ($oldPath -notlike "*$gccPath*") { [Environment]::SetEnvironmentVariable('Path', "$oldPath;$gccPath", 'User') } >> "%PS_FILE%"
    echo     Write-Host "Success: GCC added to Path." -ForegroundColor Green >> "%PS_FILE%"
    echo } >> "%PS_FILE%"

    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
    del "%PS_FILE%"
  endlocal
exit /b 0



:: Subroutine to create the mounting utility
:Install_Mount_Batch
  setlocal
    set "TARGET=%ALLBIN%\mount_fileserver.cmd"

    if not exist "%ALLBIN%" ( mkdir "%ALLBIN%" )

    echo [INFO] Creating mount utility: %TARGET%

    echo @echo off > "%TARGET%"
    echo :: Map/Mount the fileserver to C:\mnt\fileserver >> "%TARGET%"
    echo :: Usage: mount_fileserver.cmd [USERNAME] [PASSWORD] [SERVER_IP] [SERVER_SHARE] >> "%TARGET%"
    echo :: ver1.0: yan.akira@gmail.com 2026.1.12  Only need admin if LINK not created >> "%TARGET%"
    echo. >> "%TARGET%"
    echo set "REMOTE_HOST=172.16.28.1" >> "%TARGET%"
    echo set "REMOTE_SHARE=fileserver" >> "%TARGET%"
    echo set "LOCAL_TOP=C:\mnt" >> "%TARGET%"
    echo. >> "%TARGET%"
    echo set "REMOTE_USER=%%1" >> "%TARGET%"
    echo set "REMOTE_PASS=%%2" >> "%TARGET%"
    echo :: optional: override server IP / share via 3rd and 4th args >> "%TARGET%"
    echo if not "%%3"=="" set "REMOTE_HOST=%%3" >> "%TARGET%"
    echo if not "%%4"=="" set "REMOTE_SHARE=%%4" >> "%TARGET%"
    echo set "LOCAL_FOLDER=%%REMOTE_SHARE%%" >> "%TARGET%"
    echo. >> "%TARGET%"
    echo if "%%REMOTE_PASS%%"=="" ( >> "%TARGET%"
    echo     echo [ERROR] Usage: %%~nx0 USERNAME PASSWORD [SERVER_IP] [SERVER_SHARE] >> "%TARGET%"
    echo     exit /b 1 >> "%TARGET%"
    echo ) >> "%TARGET%"
    echo. >> "%TARGET%"
    echo set "LOCAL_PATH=%%LOCAL_TOP%%\%%LOCAL_FOLDER%%" >> "%TARGET%"
    echo set "REMOTE_PATH=\\%%REMOTE_HOST%%\%%REMOTE_SHARE%%" >> "%TARGET%"
    echo. >> "%TARGET%"
    echo :: Remove existing mapping to prevent conflicts >> "%TARGET%"
    echo net use "%%REMOTE_PATH%%" /delete /y ^>nul 2^>^&1 >> "%TARGET%"
    echo. >> "%TARGET%"
    echo :: Authenticate fresh >> "%TARGET%"
    echo net use "%%REMOTE_PATH%%" %%REMOTE_PASS%% /user:%%REMOTE_USER%% /persistent:yes ^>nul 2^>^&1 >> "%TARGET%"
    echo if %%errorlevel%% neq 0 ( >> "%TARGET%"
    echo     echo [ERROR] Failed to authenticate. >> "%TARGET%"
    echo     exit /b 1 >> "%TARGET%"
    echo ) >> "%TARGET%"
    echo. >> "%TARGET%"
    echo :: Only if LINK not created yet, need to run as Administrator >> "%TARGET%"
    echo if not exist "%%LOCAL_PATH%%" ( >> "%TARGET%"
    echo     openfiles ^>nul 2^>^&1 >> "%TARGET%"
    echo     if %%errorlevel%% neq 0 ( >> "%TARGET%"
    echo         echo [ERROR] Need to run as Administrator to create the initial link. >> "%TARGET%"
    echo         exit /b 1 >> "%TARGET%"
    echo     ) >> "%TARGET%"
    echo     if not exist "%%LOCAL_TOP%%" ( mkdir "%%LOCAL_TOP%%" ) >> "%TARGET%"
    echo     mklink /d "%%LOCAL_PATH%%" "%%REMOTE_PATH%%" ^>nul >> "%TARGET%"
    echo     dir "%%LOCAL_PATH%%" ^>nul 2^>^&1 >> "%TARGET%"
    echo     if %%errorlevel%% neq 0 ( >> "%TARGET%"
    echo         echo [ERROR] Failed to create Symbolic link. >> "%TARGET%"
    echo         exit /b 1 >> "%TARGET%"
    echo     ) >> "%TARGET%"
    echo ) >> "%TARGET%"
    echo exit /b 0 >> "%TARGET%"
  endlocal
exit /b 0
