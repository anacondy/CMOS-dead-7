@echo off
REM ===========================================================================
REM  TimeKeeper installer  -  Windows 7 SP1 (also works on 8.x/10/11)
REM
REM  Run from an elevated command prompt:   install.cmd
REM
REM  What it does, and only this:
REM    1. copies TimeKeeper.exe + TimeKeeper.dat to %ProgramFiles%\TimeKeeper
REM    2. registers two scheduled tasks running as SYSTEM at highest privileges
REM         TimeKeeper        at boot       (the one that fixes a dead-CMOS clock)
REM         TimeKeeperLogon   at logon      (belt and braces for VMs / always-on
REM                                          boxes that never see a real boot)
REM    3. starts the boot task once, so the clock is fixed without a reboot
REM
REM  No service, no driver, no tray icon, no firewall rule, no registry keys
REM  outside the task store. uninstall.cmd reverses exactly steps 2 and 3.
REM
REM  SPDX-License-Identifier: MIT
REM ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "APPDIR=%ProgramFiles%\TimeKeeper"
set "TASK1=TimeKeeper"
set "TASK2=TimeKeeperLogon"

REM ---------------------------------------------------------------- admin ---
net session >nul 2>nul
if errorlevel 1 (
  echo [X] This must run from an elevated command prompt.
  echo     Start ^> cmd ^> right-click ^> Run as administrator, then try again.
  exit /b 2
)

REM ------------------------------------------------------------- bitness ---
set "EXE=TimeKeeper.exe"
if not "%PROCESSOR_ARCHITECTURE%"=="AMD64" goto :check_arm
set "NATIVE=TimeKeeper64.exe"
goto :pick
:check_arm
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if defined ProgramFiles(x86) set "NATIVE=TimeKeeper64.exe"
if not defined NATIVE set "NATIVE=TimeKeeper32.exe"
:pick
if exist "%~dp0bin\!NATIVE!" ( set "EXESRC=%~dp0bin\!NATIVE!" & goto :have_exe )
if exist "%~dp0!NATIVE!"      ( set "EXESRC=%~dp0!NATIVE!"    & goto :have_exe )
if exist "%~dp0bin\TimeKeeper.exe"  ( set "EXESRC=%~dp0bin\TimeKeeper.exe" & goto :have_exe )
if exist "%~dp0TimeKeeper.exe"      ( set "EXESRC=%~dp0TimeKeeper.exe"      & goto :have_exe )
echo [X] No TimeKeeper binary found next to this script.
echo     Build it first:  build.bat    ^(or download the release zip^)
exit /b 1
:have_exe
echo [.] binary : !EXESRC!

REM --------------------------------------------------------------- copy ---
if not exist "%APPDIR%" mkdir "%APPDIR%"
if not exist "%APPDIR%" (
  echo [X] could not create %APPDIR%
  exit /b 1
)
copy /y "!EXESRC!" "%APPDIR%\TimeKeeper.exe" >nul
if errorlevel 1 ( echo [X] copy of TimeKeeper.exe failed & exit /b 1 )

REM The .dat is the offline fallback date. If one is already installed, keep it:
REM it is newer than whatever shipped with this archive and self-updates itself.
if exist "%~dp0TimeKeeper.dat" (
  if not exist "%APPDIR%\TimeKeeper.dat" copy /y "%~dp0TimeKeeper.dat" "%APPDIR%\TimeKeeper.dat" >nul
)
if not exist "%APPDIR%\TimeKeeper.dat" (
  date /t >nul 2>nul
  >"%APPDIR%\TimeKeeper.dat" echo # TimeKeeper offline fallback date
  >>"%APPDIR%\TimeKeeper.dat" echo 2026-09-01
  echo [.] seeded TimeKeeper.dat with the shipped default
)

REM --------------------------------------------------------------- tasks ---
schtasks /create /f /tn "%TASK1%" ^
  /tr "\"%APPDIR%\TimeKeeper.exe\"" ^
  /sc onstart /ru SYSTEM /rl HIGHEST /delay 0000:10 >nul 2>nul
if errorlevel 1 (
  REM /delay is not understood by Task Scheduler 1.0; retry without it.
  schtasks /create /f /tn "%TASK1%" /tr "\"%APPDIR%\TimeKeeper.exe\"" ^
           /sc onstart /ru SYSTEM /rl HIGHEST
  if errorlevel 1 ( echo [X] could not register the boot task & exit /b 1 )
  echo [.] boot task registered ^(no start-delay: unsupported on this OS^)
) else (
  echo [.] boot task registered ^(delay 10 s^)
)

schtasks /create /f /tn "%TASK2%" /tr "\"%APPDIR%\TimeKeeper.exe\"" ^
         /sc onlogon /ru SYSTEM /rl HIGHEST >nul 2>nul
if errorlevel 1 (
  echo [!] the logon task could not be registered - the boot task is the one that matters
) else (
  echo [.] logon task registered
)

REM ------------------------------------------------------------- run now ---
echo [.] correcting the clock right now ...
schtasks /run /tn "%TASK1%" >nul 2>nul
if errorlevel 1 (
  "%APPDIR%\TimeKeeper.exe" /debug
) else (
  timeout /t 6 /nobreak >nul 2>nul
)

echo.
echo --- result -------------------------------------------------------------
schtasks /query /tn "%TASK1%" /fo LIST /v 2>nul | findstr /i /c:"Task To Run" /c:"Next Run Time" /c:"Last Run Time" /c:"Last Result" /c:"Run As User"
echo.
echo clock now :
echo           %DATE% %TIME%
echo log       : %ALLUSERSPROFILE%\TimeKeeper\timekeeper.log
echo verify    : type "%ALLUSERSPROFILE%\TimeKeeper\timekeeper.log"
echo           : "%APPDIR%\TimeKeeper.exe" /dry-run
echo uninstall : "%~dp0uninstall.cmd"
echo.
echo [OK] installed. Reboot (or just wait for the next cold boot) to confirm.
endlocal
exit /b 0
