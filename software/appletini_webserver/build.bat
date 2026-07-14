@echo off
setlocal
cd /d "%~dp0"

where cl65 >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl65 was not found in PATH.
    exit /b 1
)
if not exist ip65\ip65_web.lib (
    echo ERROR: missing ip65\ip65_web.lib
    exit /b 1
)
if not exist build mkdir build

call :build A2WEBSRV webserver.c
if errorlevel 1 exit /b 1
call :build A2BROWSE browser.c
if errorlevel 1 exit /b 1

echo Built build\A2WEBSRV.SYSTEM and build\A2BROWSE.SYSTEM
exit /b 0

:build
cl65 -t apple2 --cpu 6502 -Oirs --warnings-as-errors ^
    -C apple2-system.cfg ^
    -m build\%1.map ^
    -l build\%1.lst ^
    -o build\%1.SYSTEM ^
    %2 appletini_net.c ip65\ip65_web.lib
exit /b %errorlevel%
