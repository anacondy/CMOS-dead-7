@echo off
REM ===========================================================================
REM  TimeKeeper uninstaller  -  removes exactly what install.cmd created.
REM
REM  Run elevated:   uninstall.cmd            (keeps the log + .dat)
REM                  uninstall.cmd /purge     (also deletes the install dir)
REM
REM  SPDX-License-Identifier: MIT
REM ===========================================================================
setlocal enabledelayedexpansion
set "APPDIR=%ProgramFiles%\TimeKeeper"
set "DATADIR=%ALLUSERSPROFILE%\TimeKeeper"
set "PURGE="
if /i "%~1"=="/purge" set "PURGE=1"

net session >nul 2>nul
if errorlevel 1 (
  echo [X] Run this from an elevated command prompt.
  exit /b 2
)

schtasks /delete /f /tn "TimeKeeper"      >nul 2>nul && echo [.] removed task TimeKeeper      || echo [!] TimeKeeper not present
schtasks /delete /f /tn "TimeKeeperLogon" >nul 2>nul && echo [.] removed task TimeKeeperLogon || echo [!] TimeKeeperLogon not present

if defined PURGE (
  if exist "%APPDIR%\TimeKeeper.exe"   del /f /q "%APPDIR%\TimeKeeper.exe"   && echo [.] deleted TimeKeeper.exe
  if exist "%APPDIR%\TimeKeeper.dat"   del /f /q "%APPDIR%\TimeKeeper.dat"   && echo [.] deleted TimeKeeper.dat
  if exist "%APPDIR%"                  rmdir "%APPDIR%" 2>nul                && echo [.] removed %APPDIR%
  if exist "%DATADIR%\timekeeper.log"  del /f /q "%DATADIR%\timekeeper.log"  && echo [.] deleted the log
  if exist "%DATADIR%"                 rmdir "%DATADIR%" 2>nul               && echo [.] removed %DATADIR%
) else (
  echo [.] left %APPDIR% and the log in place. Use "uninstall.cmd /purge" to remove them.
)

echo.
echo The system clock itself is untouched - set it however you like:
echo     w32tm /resync /force      (if you use a time server)
echo     date ^& time                (by hand)
endlocal
exit /b 0
