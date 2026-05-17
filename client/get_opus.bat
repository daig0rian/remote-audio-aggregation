@echo off
:: get_opus.bat - Downloads opus 1.3.1 and builds deps\libopus.lib (MSVC x64 /MT static)
:: Uses vcvars64.bat so INCLUDE/LIB are set correctly without manual path gymnastics.

setlocal enabledelayedexpansion

set OPUS_VER=1.3.1
set OPUS_SRC=opus-%OPUS_VER%
set ROOT=%~dp0
set DEPS_DIR=%ROOT%deps
set OBJ_DIR=%ROOT%opus_build_tmp

if exist "%DEPS_DIR%\libopus.lib" (
    echo libopus.lib already exists, skipping.
    exit /b 0
)

:: ---- Find MSVC via vswhere ----
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% ( echo ERROR: vswhere.exe not found. & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if not defined VS_PATH ( echo ERROR: No VS with VC++ tools found. & exit /b 1 )

:: Set up MSVC x64 environment (sets PATH, INCLUDE, LIB correctly)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

echo MSVC environment ready.
echo.

:: ---- Download + extract ----
if not exist "%ROOT%%OPUS_SRC%" (
    if not exist "%ROOT%opus-%OPUS_VER%.tar.gz" (
        echo Downloading opus %OPUS_VER% ...
        powershell -Command "Invoke-WebRequest -Uri 'https://downloads.xiph.org/releases/opus/opus-%OPUS_VER%.tar.gz' -OutFile '%ROOT%opus-%OPUS_VER%.tar.gz' -UseBasicParsing"
        if errorlevel 1 ( echo Download failed. & exit /b 1 )
    )
    echo Extracting ...
    pushd "%ROOT%"
    tar -xzf opus-%OPUS_VER%.tar.gz
    if errorlevel 1 ( popd & echo Extraction failed. & exit /b 1 )
    popd
)

:: ---- Compile from opus source dir (avoids space-in-path issues for source globs) ----
if exist "%OBJ_DIR%" rmdir /s /q "%OBJ_DIR%"
mkdir "%OBJ_DIR%"

pushd "%ROOT%%OPUS_SRC%"

set INC=/I"." /I"include" /I"celt" /I"silk" /I"silk\float"
set DEF=/DOPUS_BUILD /DUSE_ALLOCA /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE
set CFLAGS=/c /nologo /O2 /MT /W0

echo Compiling celt ...
for %%f in (celt\*.c) do (
    cl %CFLAGS% %DEF% %INC% /Fo"%OBJ_DIR%\c_%%~nf.obj" "%%f"
    if errorlevel 1 ( popd & echo FAILED: %%f & exit /b 1 )
)

echo Compiling silk ...
for %%f in (silk\*.c) do (
    cl %CFLAGS% %DEF% %INC% /Fo"%OBJ_DIR%\s_%%~nf.obj" "%%f"
    if errorlevel 1 ( popd & echo FAILED: %%f & exit /b 1 )
)

echo Compiling silk\float ...
for %%f in (silk\float\*.c) do (
    cl %CFLAGS% %DEF% %INC% /Fo"%OBJ_DIR%\sf_%%~nf.obj" "%%f"
    if errorlevel 1 ( popd & echo FAILED: %%f & exit /b 1 )
)

echo Compiling src ...
for %%f in (opus.c opus_decoder.c opus_encoder.c opus_multistream.c opus_multistream_encoder.c opus_multistream_decoder.c repacketizer.c analysis.c mlp.c mlp_data.c) do (
    cl %CFLAGS% %DEF% %INC% /Fo"%OBJ_DIR%\src_%%~nf.obj" "src\%%f"
    if errorlevel 1 ( popd & echo FAILED: src\%%f & exit /b 1 )
)

popd

:: ---- Package ----
if not exist "%DEPS_DIR%"         mkdir "%DEPS_DIR%"
if not exist "%DEPS_DIR%\include" mkdir "%DEPS_DIR%\include"

lib /nologo /OUT:"%DEPS_DIR%\libopus.lib" "%OBJ_DIR%\*.obj"
if errorlevel 1 ( echo lib.exe failed. & exit /b 1 )

xcopy /y "%ROOT%%OPUS_SRC%\include\*.h" "%DEPS_DIR%\include\" >nul

rmdir /s /q "%OBJ_DIR%"

echo.
echo libopus.lib built: %DEPS_DIR%\libopus.lib
