@echo off
python "%~dp0trit_tool.py" fuzz %*
exit /b %ERRORLEVEL%
