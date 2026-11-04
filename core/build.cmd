@echo off
rem Build relief_fix (static lib + DLL + CLI) with MinGW-w64 gcc 15 (WinLibs UCRT).
setlocal
rem gcc is taken from PATH; set MINGW_BIN to a MinGW-w64 bin folder to use that one instead.
if defined MINGW_BIN set "PATH=%MINGW_BIN%;%PATH%"
set "CFLAGS=-O2 -std=c11 -Wall -Wextra -ffp-contract=off"
cd /d "%~dp0"
gcc %CFLAGS% -c relief_fix.c -o relief_fix.o || goto fail
ar rcs librelief_fix.a relief_fix.o || goto fail
gcc %CFLAGS% -shared -static-libgcc relief_fix.c -o relief_fix.dll -Wl,--out-implib,librelief_fix.dll.a || goto fail
gcc %CFLAGS% -static-libgcc rf_cli.c relief_fix.o -o rf_cli.exe -lshell32 || goto fail
if exist tests\rf_selftest.c (
  gcc %CFLAGS% -static-libgcc -I. tests\rf_selftest.c -o tests\rf_selftest.exe -lshell32 || goto fail
)
echo build ok
exit /b 0
:fail
echo build FAILED
exit /b 1
