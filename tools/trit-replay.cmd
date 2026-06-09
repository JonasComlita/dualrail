@echo off
python "%~dp0trit_tool.py" replay %*
exit /b %ERRORLEVEL%
