@echo off
python "%~dp0trit_tool.py" export-diagnostics %*
exit /b %ERRORLEVEL%
