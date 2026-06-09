$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "trit_tool.py"
python $tool doctor @args
exit $LASTEXITCODE
