@echo off
python "%~dp0trit_tool.py" bench %*
exit /b %ERRORLEVEL%
