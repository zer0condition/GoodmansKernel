@echo off
setlocal EnableDelayedExpansion
rem  goodmans - one-shot build: driver + samples + gui, all staged into deploy\
rem  usage: build.cmd [Debug|Release]

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"`) do set "MSBUILD=%%i"
)
if not exist "%MSBUILD%" (
    echo [X] MSBuild not found. install Visual Studio 2022.
    exit /b 1
)

set "QT_DIR=C:\Qt\6.9.3\msvc2022_64"
if not exist "%QT_DIR%\bin\Qt6Core.dll" (
    echo [!] Qt not at %QT_DIR% - gui step will be skipped
    set "SKIP_GUI=1"
)

rem stop driver silently so deploy\Goodmans.sys isn't locked during copy
sc stop Goodmans >nul 2>&1

echo.
echo [1/4] driver + samples + cli + windbg ext
"%MSBUILD%" "%ROOT%\Goodmans.sln" /p:Configuration=%CFG% /p:Platform=x64 /m /nologo /v:m
if errorlevel 1 (
    echo [X] sln build failed
    exit /b 1
)

if defined SKIP_GUI goto :after_gui

echo.
echo [2/4] gui-qt configure
if not exist "%ROOT%\gui-qt\build\CMakeCache.txt" (
    cmake -S "%ROOT%\gui-qt" -B "%ROOT%\gui-qt\build" -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QT_DIR%" >nul
    if errorlevel 1 (
        echo [X] cmake configure failed
        exit /b 1
    )
)

echo.
echo [3/4] gui-qt build
cmake --build "%ROOT%\gui-qt\build" --config Release --parallel
if errorlevel 1 (
    echo [X] gui-qt build failed
    exit /b 1
)

:after_gui

echo.
echo [4/4] summary
echo.
echo deploy tree:
echo   %ROOT%\deploy\Goodmans.sys
echo   %ROOT%\deploy\Goodmans.inf
echo   %ROOT%\deploy\GoodmansTest.cer / .pfx
echo   %ROOT%\deploy\install.cmd / uninstall.cmd / gen_cert.cmd
echo   %ROOT%\deploy\gui\goodmans-gui.exe + Qt DLLs + toolkit.wasm
echo   %ROOT%\deploy\features\toolkit.wasm + process_tracer.wasm
echo   %ROOT%\deploy\samples\*.wasm  (demo guests)
echo.
echo to install:
echo   1. cd deploy
echo   2. gen_cert.cmd    (first time only, then run as Admin)
echo   3. install.cmd     (as Admin)
echo   4. gui\goodmans-gui.exe

endlocal
