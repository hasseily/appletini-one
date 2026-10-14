@echo off
setlocal

REM Program tagged firmware at its product's fixed multiboot flash offset.
REM Use the golden SD/UART updater for transactional metadata and XLNX-last installation.
REM Usage:
REM   program_firmware_slot.bat [path_to_firmware_bin]
REM Default:
REM   ..\FIRMWARE.BIN

set IMAGE=%~1
if "%IMAGE%"=="" set IMAGE=.\FIRMWARE.BIN

set FSBL=.\vitis_workspace\appletini_platform\export\appletini_platform\sw\boot\fsbl.elf
REM No filename-based routing or ambiguous legacy images are accepted.
set SLOT_INFO=%TEMP%\appletini_slot_%RANDOM%%RANDOM%.txt
python "%~dp0image_manifest.py" slot "%IMAGE%" > "%SLOT_INFO%"
if errorlevel 1 (
    del /q "%SLOT_INFO%" >nul 2>nul
    echo ERROR: image is not validated tagged multiboot firmware.
    exit /b 1
)
set OFFSET=
set SLOT_CAPACITY=
for /f "usebackq tokens=1,2" %%A in ("%SLOT_INFO%") do (
    set OFFSET=%%A
    set SLOT_CAPACITY=%%B
)
del /q "%SLOT_INFO%" >nul 2>nul
if not defined OFFSET exit /b 1
set HW_URL=TCP:localhost:3121

echo.
echo ===============================================
echo Program Firmware Slot (QSPI @ %OFFSET%)
echo ===============================================
echo IMAGE: %IMAGE%
echo CAP  : %SLOT_CAPACITY%
echo Prefer the golden updater to preserve transactional boot metadata.
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

call program_flash -f "%IMAGE%" -offset %OFFSET% -flash_type qspi-x4-single -fsbl "%FSBL%" -verify -url %HW_URL%
set RC=%ERRORLEVEL%

echo.
if not "%RC%"=="0" (
    echo FAILED with errorlevel %RC%
    exit /b %RC%
)
echo SUCCESS
exit /b 0
