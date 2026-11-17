@echo off
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel% equ 0 (
  py -3 -m pip install pyserial
) else (
  python -m pip install pyserial
)
pause
