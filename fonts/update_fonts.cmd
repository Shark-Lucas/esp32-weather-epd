@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_fonts.ps1" -Apply -Build
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" (
  echo Font update failed with exit code %RESULT%.
) else (
  echo Font update and firmware build completed successfully.
)
pause
exit /b %RESULT%
