@echo off
rem Builds the lib_path_planning.dll proxy with MinGW-w64 gcc (WinLibs UCRT build).
rem   build.cmd                     -> lib_path_planning.dll here, hooks from rf_dynamic.c (loads relief_fix_core.dll)
rem   build.cmd hooks.c             -> same, hooks (rf_prepare_task/rf_restore_task) from hooks.c
rem   build.cmd hooks.c out.dll     -> write the DLL somewhere else
rem lib_path_planning.def comes from gen_def.py (run it again if the original plugin changes).
rem Static libgcc/winpthread; the only runtime imports are KERNEL32 and the Windows UCRT
rem (api-ms-win-crt-*), listed by objdump at the end.
setlocal
set "HERE=%~dp0"
rem gcc is taken from PATH; set MINGW_BIN to a MinGW-w64 bin folder to use that one instead.
if defined MINGW_BIN set "PATH=%MINGW_BIN%;%PATH%"
set "HOOKS=%~1"
if "%HOOKS%"=="" set "HOOKS=%HERE%rf_dynamic.c"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%HERE%lib_path_planning.dll"

gcc -std=gnu11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0601 -I"%HERE%." -shared -o "%OUT%" ^
    "%HERE%proxy.c" "%HERE%seh_guard.S" "%HOOKS%" "%HERE%lib_path_planning.def" ^
    -static -static-libgcc -s -Wl,--dynamicbase,--nxcompat,--high-entropy-va
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo built %OUT%
objdump -p "%OUT%" | findstr /C:"DLL Name"
endlocal
exit /b 0
