@echo off
python "%~dp0trit_tool.py" inspect-image %*
exit /b %ERRORLEVEL%
