@echo off
REM ---------------------------------------------------------------------------
REM  build.bat - TimeKeeper, native Windows build (MSVC or MinGW-w64)
REM
REM    build.bat              both architectures, auto-detect toolchain
REM    build.bat x86          32-bit only        (for a 32-bit Win7 SP1 install)
REM    build.bat x64          64-bit only
REM    build.bat mingw        force MinGW-w64
REM    build.bat msvc         force Visual Studio
REM    build.bat test         host unit tests only
REM
REM  MSVC:  VS 2017/2019/2022 Build Tools, "Desktop C++" workload. The v141_xp
REM         toolset is NOT required: the code targets _WIN32_WINNT=0x0601 and
REM         uses no XP-era shims; it is only needed if you insist on the
REM         Windows-XP SupportedOS subsystem value.
REM  MinGW: MSYS2 `pacman -S mingw-w64-i686-toolchain mingw-w64-x86_64-toolchain`,
REM         or the standalone winlibs builds. MinGW is the recommended path
REM         because -nostartfiles is what gets the binary under 80 KB.
REM
REM  SPDX-License-Identifier: MIT
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "ARCH=both"
set "FORCE="
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="x86"   set "ARCH=x86"
if /i "%~1"=="x64"   set "ARCH=x64"
if /i "%~1"=="both"  set "ARCH=both"
if /i "%~1"=="mingw" set "FORCE=mingw"
if /i "%~1"=="msvc"  set "FORCE=msvc"
if /i "%~1"=="test"  call :host_test & exit /b !ERRORLEVEL!
shift
goto :parse
:parsed

if not exist bin   mkdir bin
if not exist build mkdir build

call :host_test
if not "!ERRORLEVEL!"=="0" exit /b 1

set "TC="
if /i not "%FORCE%"=="mingw" call :find_msvc
if defined TC goto :do_msvc
if /i "%FORCE%"=="msvc" (
  echo [X] Visual Studio requested but not found
  exit /b 2
)
where x86_64-w64-mingw32-gcc >nul 2>nul
if errorlevel 1 (
  echo [X] no toolchain found. Install VS Build Tools ^(C++^) or MinGW-w64.
  exit /b 2
)
call :build_mingw 32
if not "!ERRORLEVEL!"=="0" exit /b 1
call :build_mingw 64
if not "!ERRORLEVEL!"=="0" exit /b 1
goto :report

:do_msvc
if "%ARCH%"=="x86"  call :build_msvc x86
if "%ARCH%"=="x64"  call :build_msvc x64
if "%ARCH%"=="both" ( call :build_msvc x86 & call :build_msvc x64 )
if not "!ERRORLEVEL!"=="0" exit /b 1
goto :report

REM ---------------------------------------------------------------------------
:host_test
where gcc >nul 2>nul
if errorlevel 1 (
  echo [.] gcc not on PATH - skipping the host unit tests
  exit /b 0
)
echo [.] host unit tests (gcc, ASan+UBSan)
gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer ^
    -Wall -Wextra -Wshadow -Isrc -I. ^
    tests\host_test.c src\timeutil.c src\verdict.c src\selftest.c ^
    src\sntp_proto.c src\fallback_proto.c -o build\host_test.exe
if errorlevel 1 ( echo [X] test compile failed & exit /b 1 )
build\host_test.exe
if errorlevel 1 ( echo [X] tests FAILED & exit /b 1 )
echo [OK] unit tests passed
exit /b 0

:find_msvc
set "VSVARS="
for %%Y in (2022 2023 2019 2017) do (
  for %%E in (BuildTools Community Professional Enterprise) do (
    set "C=!ProgramFiles(x86)!\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvarsall.bat"
    if exist "!C!" set "VSVARS=!C!"
    set "C=!ProgramFiles!\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvarsall.bat"
    if exist "!C!" set "VSVARS=!C!"
  )
)
if defined VSVARS (
  echo [.] MSVC found: !VSVARS!
  set "TC=msvc"
)
exit /b 0

:build_msvc
REM %1 = x86 | x64
set "PLAT=%~1"
echo [.] cl /O1 /Os /GL /MT  (%PLAT%)
call "%VSVARS%" %PLAT% >nul
if errorlevel 1 ( echo [X] vcvarsall failed & exit /b 1 )

rc /nologo /Isrc /Fobuild\version_%PLAT%.res src\version.rc
if errorlevel 1 ( echo [X] rc.exe failed & exit /b 1 )

if not exist build\obj_%PLAT% mkdir build\obj_%PLAT%
cl /nologo /c /O1 /Os /GL /MT /W4 /EHs-c- /GR- /Gs /Gy ^
   /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 ^
   /DUNICODE /D_UNICODE /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /I. /Fobuild\obj_%PLAT%\ ^
   src\main.c src\timeutil.c src\verdict.c src\selftest.c src\sntp.c ^
   src\sntp_proto.c src\http_date.c src\fallback.c src\fallback_proto.c ^
   src\privilege.c src\log.c src\winutil.c src\install.c
if errorlevel 1 ( echo [X] cl failed & exit /b 1 )

link /nologo /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF /LTCG ^
     /STACK:0x10000,0x2000 /MANIFEST:NO ^
     /OUT:bin\TimeKeeper_%PLAT%.tmp.exe build\obj_%PLAT%\*.obj build\version_%PLAT%.res ^
     kernel32.lib advapi32.lib ws2_32.lib wininet.lib user32.lib
if errorlevel 1 ( echo [X] link failed & exit /b 1 )
move /y bin\TimeKeeper_%PLAT%.tmp.exe bin\TimeKeeper%PLAT:x86=32%.exe >nul
if "%PLAT%"=="x64" move /y bin\TimeKeeper_x64.tmp.exe bin\TimeKeeper64.exe >nul
echo [OK] bin\TimeKeeper%PLAT:x86=32%.exe
exit /b 0

:build_mingw
REM %1 = 32 | 64
set "A=%~1"
if "%A%"=="32" (
  set "CCX=i686-w64-mingw32-gcc"
  set "WRS=i686-w64-mingw32-windres"
  set "STP=i686-w64-mingw32-strip"
  set "ARCHF=-march=i686"
) else (
  set "CCX=x86_64-w64-mingw32-gcc"
  set "WRS=x86_64-w64-mingw32-windres"
  set "STP=x86_64-w64-mingw32-strip"
  set "ARCHF="
)
where !CCX! >nul 2>nul
if errorlevel 1 ( echo [!] !CCX! not found - skipped ^(& exit /b 0 ) )
echo [.] !CCX!

pushd build
copy /y ..\src\app.manifest app.manifest >nul
!WRS! -I..\src -I. -D_WIN32_WINNT=0x0601 --output-format=coff ..\src\version.rc version_%A%.o
if errorlevel 1 ( popd & echo [X] windres failed & exit /b 1 )
popd

!CCX! -std=c11 -Os -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables ^
  -ffunction-sections -fdata-sections -fno-stack-protector !ARCHF! ^
  -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes ^
  -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000 ^
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DSTRICT -DUNICODE -D_UNICODE ^
  -DTK_NOCRT -DTK_SELFTEST_COMPACT ^
  -nostartfiles -static -static-libgcc ^
  -Wl,--gc-sections -Wl,--build-id=none -Wl,--nxcompat -Wl,--dynamicbase ^
  -Wl,--file-alignment,512 -Wl,--subsystem,windows ^
  -Wl,--major-os-version,6 -Wl,--minor-os-version,1 ^
  -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1 ^
  -Isrc -I. -o bin\TimeKeeper%A%.exe ^
  src\crt_start.c src\main.c src\timeutil.c src\verdict.c src\selftest.c ^
  src\sntp.c src\sntp_proto.c src\http_date.c src\fallback.c src\fallback_proto.c ^
  src\privilege.c src\log.c src\winutil.c src\install.c build\version_%A%.o ^
  -lkernel32 -ladvapi32 -lws2_32 -lwininet -luser32
if errorlevel 1 ( echo [X] build failed for %A%-bit & exit /b 1 )
!STP! --strip-all bin\TimeKeeper%A%.exe
echo [OK] bin\TimeKeeper%A%.exe
exit /b 0

:report
echo.
echo artifacts:
for %%F in (bin\TimeKeeper32.exe bin\TimeKeeper64.exe) do if exist "%%~F" (
  echo   %%~nxF  %%~zF bytes
)
echo.
echo budget: target ^< 81920 bytes, hard limit 153600 bytes.
endlocal
exit /b 0
