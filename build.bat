@echo off
REM Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
REM SPDX-License-Identifier: MIT
REM Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

REM ============================================================================
REM SpoutPlugin release launcher.
REM
REM Forwards every argument to build.ps1, captures PowerShell's exit code
REM IMMEDIATELY (before pause, which would otherwise clobber ERRORLEVEL), and
REM returns that same code to the caller. A failed release therefore fails this
REM batch file too, which is what lets CI or a wrapper script detect it.
REM
REM Set SPOUT_BUILD_NO_PAUSE=1 to skip the interactive pause.
REM
REM Two entry points behind one launcher:
REM
REM   build.bat -TrackD [...]   -> Scripts\Invoke-TrackD.ps1, the validation pipeline
REM   build.bat <anything else> -> build.ps1, the release build
REM
REM Usage:
REM   build.bat -FullMatrix -OutputRoot "E:\Patreon\UEPLUGINS\SpoutBuild"
REM   build.bat -Partial -EngineVersions 5.3,5.8 -OutputRoot "D:\staging"
REM   build.bat -TrackD
REM   build.bat -TrackD -BoundaryOnly
REM   build.bat -TrackD -FullMatrix
REM   build.bat -TrackD -Performance
REM   build.bat -TrackD -CleanSource
REM   build.bat -TrackD -All
REM ============================================================================

setlocal

if "%~1"=="" (
    echo build.bat requires arguments.
    echo.
    echo Release:
    echo   build.bat -FullMatrix -OutputRoot "E:\Patreon\UEPLUGINS\SpoutBuild"
    echo   build.bat -Partial -EngineVersions 5.3,5.8 -OutputRoot "D:\staging"
    echo.
    echo Validation:
    echo   build.bat -TrackD                  fast: docs + automation on the engine floors
    echo   build.bat -TrackD -BoundaryOnly    adds UE 5.3 and 5.8 packaged hosts
    echo   build.bat -TrackD -FullMatrix      the whole declared engine matrix
    echo   build.bat -TrackD -Performance     adds the performance baselines
    echo   build.bat -TrackD -CleanSource     adds clean-source-copy reproducibility
    echo   build.bat -TrackD -All             everything
    echo.
    echo build.ps1 refuses to run without an explicit -FullMatrix or -Partial mode
    echo and an -OutputRoot, so a partial release cannot be produced by accident.
    if not "%SPOUT_BUILD_NO_PAUSE%"=="1" pause
    exit /b 2
)

REM Route -TrackD to the validation pipeline, with the flag consumed so the remaining arguments are
REM forwarded verbatim.
REM
REM The arguments are re-collected one at a time rather than forwarded with %*: SHIFT does not
REM affect %*, so "%*" would still contain the -TrackD this block just consumed. The loop also
REM preserves each argument's original quoting, which matters for -OutputRoot paths with spaces.
set "SPOUT_TARGET=%~dp0build.ps1"
set "SPOUT_ARGS="

if /I not "%~1"=="-TrackD" goto :collect
set "SPOUT_TARGET=%~dp0Scripts\Invoke-TrackD.ps1"
shift

:collect
if "%~1"=="" goto :run
set "SPOUT_ARGS=%SPOUT_ARGS% %1"
shift
goto :collect

:run

REM Prefer PowerShell 7+ when it is installed. Windows PowerShell 5.1 is a supported fallback;
REM build.ps1 avoids optional modules precisely so both editions work.
where pwsh >nul 2>&1
if %ERRORLEVEL%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%SPOUT_TARGET%" %SPOUT_ARGS%
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SPOUT_TARGET%" %SPOUT_ARGS%
)

REM Capture first: any command in between, including pause, resets ERRORLEVEL.
set "PS_EXIT=%ERRORLEVEL%"

if not "%SPOUT_BUILD_NO_PAUSE%"=="1" pause

exit /b %PS_EXIT%
