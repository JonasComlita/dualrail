$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "trit_tool.py"
python $tool inspect-image @args
exit $LASTEXITCODE
