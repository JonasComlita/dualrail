$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "trit_tool.py"
python $tool export-diagnostics @args
exit $LASTEXITCODE
