@echo off
echo ============================================
echo   HALE Process Monitor - Build Script
echo ============================================
echo.
echo Compiling with static linking (no external DLLs needed)...
echo.

g++ -O2 -o monitor.exe monitor.cpp -mwindows -static -lcomctl32 -luxtheme -lpsapi -ldwmapi

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Build complete: monitor.exe
    echo   - All libraries statically linked
    echo   - No external DLLs required
    echo.
) else (
    echo.
    echo [ERROR] Build failed with error code %ERRORLEVEL%
    echo.
)
pause
