$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "trit_tool.py"
python $tool compact-disk @args
exit $LASTEXITCODE
