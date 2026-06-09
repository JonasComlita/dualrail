@echo off
python "%~dp0trit_tool.py" build-image %*
exit /b %ERRORLEVEL%
