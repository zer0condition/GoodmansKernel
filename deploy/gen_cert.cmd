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

set "CER=%HERE%\GoodmansTest.cer"
set "PFX=%HERE%\GoodmansTest.pfx"

if exist "%CER%" if exist "%PFX%" (
    echo [*] cert already exists: %CER%
    echo     delete both files first if you want to regenerate.
    exit /b 0
)

echo [*] generating self-signed code-signing cert
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=GoodmansTest' -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -NotAfter (Get-Date).AddYears(5);" ^
    "Export-Certificate -Cert $c -FilePath '%CER%' | Out-Null;" ^
    "$pw = ConvertTo-SecureString -String 'goodmans' -Force -AsPlainText;" ^
    "Export-PfxCertificate -Cert $c -FilePath '%PFX%' -Password $pw | Out-Null;" ^
    "Write-Host ('thumbprint: ' + $c.Thumbprint)"

if errorlevel 1 (
    echo [X] cert generation failed
    exit /b 1
)

echo.
echo [+] generated:
echo     %CER%
echo     %PFX%  (password: goodmans)
echo.
echo [*] to sign a driver:
echo     signtool sign /fd sha256 /f "%PFX%" /p goodmans Goodmans.sys

endlocal
