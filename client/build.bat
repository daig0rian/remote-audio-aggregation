@echo off
:: build.bat - Builds raa-client.exe (tray app)
:: Uses vcvars64.bat so INCLUDE/LIB/PATH are set correctly.

setlocal enabledelayedexpansion

set ROOT=%~dp0

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
if not exist "%ROOT%deps\libopus.lib" (
    echo libopus.lib not found. Running get_opus.bat ...
    call "%ROOT%get_opus.bat"
    if errorlevel 1 ( echo get_opus.bat failed. & exit /b 1 )
    echo.
)

:: ---- Compile resource file ----
pushd "%ROOT%"

rc /nologo /fo raa-client.res raa-client.rc
if errorlevel 1 ( popd & echo RC compile FAILED. & exit /b 1 )

:: ---- Compile raa-client.exe ----
cl ^
    /nologo /W3 /O2 /EHsc /MT /utf-8 ^
    /I"deps\include" ^
    src\raa-client.cpp ^
    raa-client.res ^
    /link ^
    deps\libopus.lib ^
    mmdevapi.lib ole32.lib oleaut32.lib uuid.lib ws2_32.lib rpcrt4.lib runtimeobject.lib ^
    user32.lib shell32.lib comctl32.lib gdi32.lib advapi32.lib comdlg32.lib ^
    /SUBSYSTEM:WINDOWS ^
    /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
    /OUT:raa-client.exe

if errorlevel 1 (
    popd
    echo.
    echo Build FAILED.
    exit /b 1
)

popd

echo.
echo Build OK: client\raa-client.exe
echo.
echo The app runs in the system tray. Double-click the tray icon to open settings.
