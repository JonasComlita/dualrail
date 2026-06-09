@echo off
python "%~dp0trit_tool.py" run %*
exit /b %ERRORLEVEL%
