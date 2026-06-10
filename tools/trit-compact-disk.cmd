@echo off
python "%~dp0trit_tool.py" compact-disk %*
exit /b %ERRORLEVEL%
