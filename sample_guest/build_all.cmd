@echo off
setlocal
cd /d "%~dp0"
set CLANG=%LLVM_HOME%\bin\clang.exe
if not exist "%CLANG%" set CLANG=C:\Program Files\LLVM\bin\clang.exe
if not exist "%CLANG%" (
    echo [X] clang not found. install LLVM or set LLVM_HOME.
    exit /b 1
)

set CFLAGS=-O2 --target=wasm32 -nostdlib -fno-builtin -I..\guest_sdk
set LFLAGS=-Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined -Wl,--strip-all

call :one sample_guest
call :one ffi_demo
call :one pslist_dumper
call :one handle_stripper
call :one infinity_hook

exit /b 0

:one
echo [*] %~1
"%CLANG%" %CFLAGS% %LFLAGS% -o %~1.wasm %~1.c
exit /b 0
