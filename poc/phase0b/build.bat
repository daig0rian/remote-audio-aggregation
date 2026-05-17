@echo off
:: Phase 0B build script
:: Finds MSVC and Windows SDK automatically, then compiles wasapi_loopback.cpp

setlocal enabledelayedexpansion

:: Find MSVC via vswhere
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo ERROR: vswhere.exe not found. Install Visual Studio Build Tools.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set VS_PATH=%%i
)

if not defined VS_PATH (
    echo ERROR: No VS installation with VC++ tools found.
    echo Run the Visual Studio Installer and add "Desktop development with C++" workload.
    exit /b 1
)

:: Find latest MSVC version
for /f "delims=" %%v in ('dir /b /ad "%VS_PATH%\VC\Tools\MSVC" 2^>nul') do set MSVC_VER=%%v
set MSVC_BIN=%VS_PATH%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64

:: Find latest Windows SDK
for /f "tokens=*" %%k in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do set WIN_KITS_LINE=%%k
for /f "tokens=3*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do set WIN_KITS=%%a %%b
:: Trim trailing space
if defined WIN_KITS set WIN_KITS=%WIN_KITS: =%

:: Alternatively try 32-bit registry view
if not defined WIN_KITS (
    for /f "tokens=3*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do set WIN_KITS=%%a %%b
)

if not defined WIN_KITS (
    echo ERROR: Windows SDK not found in registry.
    echo Install "Windows 11 SDK" from the Visual Studio Installer.
    exit /b 1
)

:: Find latest SDK version
for /f "delims=" %%s in ('dir /b /ad "%WIN_KITS%Include" 2^>nul') do set SDK_VER=%%s

set SDK_INC=%WIN_KITS%Include\%SDK_VER%
set SDK_LIB=%WIN_KITS%Lib\%SDK_VER%

set CL=%MSVC_BIN%\cl.exe
set LINK=%MSVC_BIN%\link.exe

echo MSVC: %MSVC_VER%
echo SDK:  %SDK_VER%
echo.

:: Compile
"%CL%" ^
    /nologo /W3 /O2 /EHsc ^
    /I"%MSVC_BIN%\..\..\..\..\include" ^
    /I"%SDK_INC%\um" ^
    /I"%SDK_INC%\shared" ^
    /I"%SDK_INC%\ucrt" ^
    wasapi_loopback.cpp ^
    /link ^
    /LIBPATH:"%SDK_LIB%\um\x64" ^
    /LIBPATH:"%SDK_LIB%\ucrt\x64" ^
    /LIBPATH:"%VS_PATH%\VC\Tools\MSVC\%MSVC_VER%\lib\x64" ^
    ole32.lib oleaut32.lib uuid.lib mmdevapi.lib ^
    /OUT:wasapi_loopback.exe

if errorlevel 1 (
    echo.
    echo Build FAILED.
    exit /b 1
)

echo.
echo Build OK: wasapi_loopback.exe
echo Run: wasapi_loopback.exe
