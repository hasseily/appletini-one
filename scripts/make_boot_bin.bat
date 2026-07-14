@echo off
setlocal EnableExtensions

REM Create golden BOOT image (FSBL + bootloader)
REM Usage:
REM   make_boot_bin.bat [OUT_BIN] [FSBL_ELF] [BOOTLOADER_ELF]
REM Defaults:
REM   OUT_BIN         .\BOOT.BIN
REM   FSBL_ELF        .\vitis_workspace\appletini_platform\export\appletini_platform\sw\boot\fsbl.elf
REM   BOOTLOADER_ELF  .\vitis_workspace\bootloader\build\bootloader.elf

set OUT_BIN=%~1
if "%OUT_BIN%"=="" set OUT_BIN=.\BOOT.BIN

set FSBL_ELF=%~2
if "%FSBL_ELF%"=="" set FSBL_ELF=.\vitis_workspace\appletini_platform\export\appletini_platform\sw\boot\fsbl.elf

set APP_ELF=%~3
if "%APP_ELF%"=="" set APP_ELF=.\vitis_workspace\bootloader\build\bootloader.elf

if not defined XILINX_VITIS (
    echo ERROR: XILINX_VITIS is not set
    exit /b 1
)

set "BOOTGEN=%XILINX_VITIS%\bin\bootgen.bat"
if not exist "%BOOTGEN%" set "BOOTGEN=%XILINX_VITIS%\bootgen.bat"
if not exist "%BOOTGEN%" (
    echo ERROR: bootgen.bat not found under XILINX_VITIS: %XILINX_VITIS%
    echo        Expected either %%XILINX_VITIS%%\bin\bootgen.bat or %%XILINX_VITIS%%\bootgen.bat
    exit /b 1
)

if not exist "%FSBL_ELF%" (
    echo ERROR: FSBL not found: %FSBL_ELF%
    exit /b 1
)
if not exist "%APP_ELF%" (
    echo ERROR: bootloader ELF not found: %APP_ELF%
    exit /b 1
)

set TMP_BIF=%TEMP%\appletini_golden_%RANDOM%%RANDOM%.bif

(
echo the_ROM_image:
echo {
echo     [bootloader]%FSBL_ELF%
echo     %APP_ELF%
echo }
) > "%TMP_BIF%"

echo.
echo ==========================================
echo Create Golden BOOT Image
echo ==========================================
echo OUT : %OUT_BIN%
echo FSBL: %FSBL_ELF%
echo APP : %APP_ELF%
echo BIF : %TMP_BIF%
echo.

call "%BOOTGEN%" -arch zynq -image "%TMP_BIF%" -o "%OUT_BIN%" -w on
set RC=%ERRORLEVEL%

del /q "%TMP_BIF%" >nul 2>nul

if not "%RC%"=="0" (
    echo ERROR: bootgen failed ^(errorlevel %RC%^)
    exit /b %RC%
)

echo SUCCESS: %OUT_BIN%
exit /b 0
