@echo off
python "%~dp0trit_tool.py" test %*
exit /b %ERRORLEVEL%
