@echo off
rem ============================================================================
rem NoobDawn deploy script
rem   Usage: deploy.bat [Platform] [Configuration] [DistRoot]
rem     Platform      x64 (default) or Win32
rem     Configuration Release (default) or Development
rem     DistRoot      default: <repo>\dist
rem
rem   x64 artifacts  -> <DistRoot>\<Configuration>\
rem   Win32 artifacts -> <DistRoot>\<Configuration>\x86\
rem
rem The deployed folder layout matches what noobdawn.dll expects at runtime:
rem   noobdawn.dll / noobdawncmd.exe / noobdawnshim64.dll next to each other,
rem   with 32-bit binaries under x86\ (cross-bitness capture + global hook).
rem ============================================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
set "PLATFORM=%~1"
set "CONFIG=%~2"
set "DIST=%~3"

if "%PLATFORM%"=="" set "PLATFORM=x64"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%DIST%"=="" set "DIST=%ROOT%dist"

set "MISSING=0"

if /I "%PLATFORM%"=="Win32" goto :deploy32
if /I "%PLATFORM%"=="x86" goto :deploy32
if /I "%PLATFORM%"=="x64" goto :deploy64
echo [deploy] Unknown platform "%PLATFORM%" (expected x64 or Win32)
exit /b 1

rem ----------------------------------------------------------------------------
:deploy64
set "SRC=%ROOT%x64\%CONFIG%"
set "DST=%DIST%\%CONFIG%"
if not exist "%SRC%\noobdawn.dll" (
  echo [deploy] WARNING: %SRC%\noobdawn.dll not found - build x64 %CONFIG% first.
  exit /b 0
)
echo [deploy] Copying x64 %CONFIG% artifacts -^> %DST%
if not exist "%DST%" mkdir "%DST%"

for %%F in (
  qnoobdawn.exe
  noobdawnui.exe
  noobdawn.dll
  noobdawncmd.exe
  noobdawnshim64.dll
  noobdawn.json
  d3dcompiler_47.dll
  dbghelp.dll
  symsrv.dll
  symsrv.yes
  python36.dll
  python36.zip
  _ctypes.pyd
  Qt5Core.dll
  Qt5Gui.dll
  Qt5Network.dll
  Qt5Svg.dll
  Qt5Widgets.dll
) do call :copyone "%SRC%" "%DST%" "%%F"

if exist "%SRC%\qtplugins" robocopy "%SRC%\qtplugins" "%DST%\qtplugins" /E /NFL /NDL /NJH /NJS /NP >nul
if exist "%SRC%\pymodules" robocopy "%SRC%\pymodules" "%DST%\pymodules" /E /NFL /NDL /NJH /NJS /NP >nul

rem if 32-bit binaries have also been built, refresh the x86 subfolder too
if exist "%ROOT%Win32\%CONFIG%\noobdawn.dll" (
  call :deploy32inner
) else (
  echo [deploy] NOTE: Win32 %CONFIG% binaries not found - x86 subfolder skipped.
)
goto :done

rem ----------------------------------------------------------------------------
:deploy32
call :deploy32inner
goto :done

:deploy32inner
set "SRC=%ROOT%Win32\%CONFIG%"
set "DST=%DIST%\%CONFIG%\x86"
if not exist "%SRC%\noobdawn.dll" (
  echo [deploy] WARNING: %SRC%\noobdawn.dll not found - build Win32 %CONFIG% first.
  exit /b 0
)
echo [deploy] Copying Win32 %CONFIG% artifacts -^> %DST%
if not exist "%DST%" mkdir "%DST%"

for %%F in (
  noobdawn.dll
  noobdawncmd.exe
  noobdawnshim32.dll
  d3dcompiler_47.dll
  dbghelp.dll
  symsrv.dll
  symsrv.yes
) do call :copyone "%SRC%" "%DST%" "%%F"
exit /b 0

rem ----------------------------------------------------------------------------
:copyone
if exist "%~1\%~3" (
  copy /Y "%~1\%~3" "%~2\" >nul
) else (
  echo [deploy]   missing: %~3
  set "MISSING=1"
)
exit /b 0

rem ----------------------------------------------------------------------------
:done
if "%MISSING%"=="1" (
  echo [deploy] Completed with missing files ^(see above^).
) else (
  echo [deploy] Done.
)
rem always succeed so the build is not broken by deploy-only issues
exit /b 0
