@echo off
:: build.bat - Builds raa-player.exe (tray player)
:: libopus is shared with the capture client: client\deps\libopus.lib (built by client\get_opus.bat).

setlocal enabledelayedexpansion

set ROOT=%~dp0
set OPUS_DEPS=%ROOT%..\client\deps

:: ---- Find MSVC ----
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% ( echo ERROR: vswhere.exe not found. & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if not defined VS_PATH ( echo ERROR: No VS with VC++ tools found. & exit /b 1 )

:: Set up MSVC x64 environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

echo MSVC environment ready.
echo.

:: ---- Build libopus if needed ----
if not exist "%OPUS_DEPS%\libopus.lib" (
    echo libopus.lib not found. Running client\get_opus.bat ...
    call "%ROOT%..\client\get_opus.bat"
    if errorlevel 1 ( echo get_opus.bat failed. & exit /b 1 )
    echo.
)

:: ---- Compile resource file ----
pushd "%ROOT%"

rc /nologo /fo raa-player.res raa-player.rc
if errorlevel 1 ( popd & echo RC compile FAILED. & exit /b 1 )

:: ---- Compile raa-player.exe ----
cl ^
    /nologo /W3 /O2 /EHsc /MT /utf-8 ^
    /I"..\client\deps\include" ^
    src\raa-player.cpp ^
    raa-player.res ^
    /link ^
    ..\client\deps\libopus.lib ^
    ole32.lib ws2_32.lib bcrypt.lib ^
    user32.lib shell32.lib comctl32.lib gdi32.lib advapi32.lib ^
    /SUBSYSTEM:WINDOWS ^
    /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
    /OUT:raa-player.exe

if errorlevel 1 (
    popd
    echo.
    echo Build FAILED.
    exit /b 1
)

popd

echo.
echo Build OK: player\raa-player.exe
echo.
echo The app runs in the system tray. Double-click the tray icon to open settings.
