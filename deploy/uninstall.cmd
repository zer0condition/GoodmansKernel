@echo off
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [X] must be run as Administrator.
    pause
    exit /b 1
)

sc query Goodmans >nul 2>&1
if errorlevel 1 (
    echo [*] service not present
    exit /b 0
)

echo [*] sc stop Goodmans
sc stop Goodmans >nul 2>&1

echo [*] sc delete Goodmans
sc delete Goodmans

endlocal
