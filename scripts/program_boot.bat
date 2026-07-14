@echo off
setlocal

REM Program the golden BOOT image at flash base (0x0)
REM Usage:
REM   program_golden.bat [path_to_boot_bin]
REM Default:
REM   ..\BOOT.bin

set IMAGE=%~1
if "%IMAGE%"=="" set IMAGE=.\BOOT.bin

set FSBL=.\vitis_workspace\appletini_platform\export\appletini_platform\sw\boot\fsbl.elf
set HW_URL=TCP:localhost:3121

echo.
echo ==========================================
echo Program Golden Image (QSPI @ 0x00000000)
echo ==========================================
echo IMAGE: %IMAGE%
echo FSBL : %FSBL%
echo.

if not exist "%IMAGE%" (
    echo ERROR: image file not found: %IMAGE%
    exit /b 1
)

if not exist "%FSBL%" (
    echo ERROR: FSBL not found: %FSBL%
    exit /b 1
)

set TARGET_LOG=%TEMP%\appletini_jtag_targets_%RANDOM%%RANDOM%.log
echo Checking JTAG targets on %HW_URL%...
call program_flash -jtagtargets -url %HW_URL% > "%TARGET_LOG%" 2>&1
set RC=%ERRORLEVEL%
type "%TARGET_LOG%"
if not "%RC%"=="0" (
    del /q "%TARGET_LOG%" >nul 2>nul
    echo ERROR: JTAG target query failed ^(errorlevel %RC%^)
    exit /b %RC%
)
findstr /i /c:"xc7z020" /c:"arm_dap" /c:"4ba00477" "%TARGET_LOG%" >nul
if errorlevel 1 (
    del /q "%TARGET_LOG%" >nul 2>nul
    echo.
    echo ERROR: No Zynq target detected on JTAG.
    echo        Power the card and ensure it is in programming mode, then retry.
    exit /b 2
)
del /q "%TARGET_LOG%" >nul 2>nul

call program_flash -f "%IMAGE%" -offset 0x0 -flash_type qspi-x4-single -fsbl "%FSBL%" -verify -url %HW_URL%
set RC=%ERRORLEVEL%

echo.
if not "%RC%"=="0" (
    echo FAILED with errorlevel %RC%
    exit /b %RC%
)
echo SUCCESS
exit /b 0
