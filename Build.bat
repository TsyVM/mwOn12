@echo off
setlocal

:: ─────────────────────────────────────────────────────────────────────────────
:: MWOn12 build
::
:: Produces d3d9.dll (x86, Release) for Need for Speed: Most Wanted (2005) v1.3.
::
:: Needs Visual Studio 2022 with the C++ workload, and nothing else. No DirectX
:: June 2010 SDK, no network access. If MWOn12SDK is present beside this folder
:: it is picked up automatically and plugin support is compiled in; CMake says
:: which of the two happened.
:: ─────────────────────────────────────────────────────────────────────────────

set BUILD_DIR=build_win32

cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 ( echo. & echo CMake configure failed. & pause & exit /b 1 )

cmake --build %BUILD_DIR% --config Release
if errorlevel 1 ( echo. & echo Build failed. & pause & exit /b 1 )

echo.
echo ── Build complete ──
echo   Output:  %BUILD_DIR%\Release\d3d9.dll
echo.
echo To install, copy into the folder containing speed.exe:
echo   d3d9.dll
echo   MWOn12.ini
echo.
pause
