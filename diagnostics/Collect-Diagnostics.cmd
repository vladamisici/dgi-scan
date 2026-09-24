@echo off
rem Starts Collect-Diagnostics.ps1 from this folder. Bypass applies to this one process only,
rem so it works where the execution policy is Restricted or the files came from
rem the internet, without changing any machine setting.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Diagnostics.ps1" %*
set "ST_RC=%ERRORLEVEL%"
rem Keep the window open when started by double-click, so the result can be read.
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~nx0" >nul && if "%~1"=="" pause
exit /b %ST_RC%
