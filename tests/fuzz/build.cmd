@echo off
setlocal
set CLANG=%LLVM_HOME%\bin\clang.exe
if not exist "%CLANG%" set CLANG=C:\Program Files\LLVM\bin\clang.exe
if not exist "%CLANG%" (
    echo [X] clang not found. install LLVM or set LLVM_HOME.
    exit /b 1
)

set W3=..\..\driver\wasm3

"%CLANG%" -O2 -g -fsanitize=fuzzer,address ^
    -DM3_IMPLEMENT_ERROR_STRINGS ^
    -Dd_m3HasFloat=0 ^
    -I"%W3%" ^
    fuzz_parse.c ^
    "%W3%\m3_bind.c"    "%W3%\m3_code.c"     "%W3%\m3_compile.c" ^
    "%W3%\m3_core.c"    "%W3%\m3_env.c"      "%W3%\m3_exec.c" ^
    "%W3%\m3_function.c" "%W3%\m3_info.c"    "%W3%\m3_module.c" ^
    "%W3%\m3_parse.c"   "%W3%\m3_validate.c" ^
    -o fuzz_parse.exe

if not exist corpus mkdir corpus
if exist ..\..\sample_guest\sample_guest.wasm copy /Y ..\..\sample_guest\sample_guest.wasm corpus\ >nul

echo [+] built fuzz_parse.exe. run with:
echo     fuzz_parse.exe corpus -max_len=65536 -jobs=4

endlocal
