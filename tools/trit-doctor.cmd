@echo off
python "%~dp0trit_tool.py" doctor %*
exit /b %ERRORLEVEL%
