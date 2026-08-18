@echo off
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [X] must be run as Administrator.
    pause
    exit /b 1
)

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

set "SYS=%HERE%\Goodmans.sys"
set "CER=%HERE%\GoodmansTest.cer"

if not exist "%SYS%" (
    echo [X] missing: %SYS%
    echo     build the driver and copy Goodmans.sys into this folder.
    exit /b 1
)
if not exist "%CER%" (
    echo [X] missing: %CER%
    echo     run gen_cert.cmd first to create the test cert.
    exit /b 1
)


echo [*] Debug Print Filter mask
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v DEFAULT /t REG_DWORD /d 0xFFFFFFFF /f >nul

echo [*] importing cert to Root + TrustedPublisher
certutil -addstore Root "%CER%" >nul 2>&1
certutil -addstore TrustedPublisher "%CER%" >nul 2>&1

echo [*] removing any prior Goodmans service
sc query Goodmans >nul 2>&1
if not errorlevel 1 (
    sc stop   Goodmans >nul 2>&1
    sc delete Goodmans >nul 2>&1
    timeout /t 1 /nobreak >nul
)

echo [*] sc create + start
sc create Goodmans type= kernel binPath= "%SYS%"
if errorlevel 1 ( echo [X] sc create failed & exit /b 1 )
sc start Goodmans
if errorlevel 1 ( echo [X] sc start failed. check testsigning + cert import. & exit /b 1 )

echo.
echo [+] driver running. open DbgView as admin, filter on [goodmans]
echo     then:
echo       goodmans.exe load   sample_guest.wasm
echo       goodmans.exe call   1 run_all
echo       goodmans.exe unload 1

endlocal
