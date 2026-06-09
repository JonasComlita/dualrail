$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "trit_tool.py"
python $tool fuzz @args
exit $LASTEXITCODE
