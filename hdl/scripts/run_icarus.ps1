param(
    [string[]] $FileLists = @(
        "hdl/trit_gate_tb.f",
        "hdl/trit_lane_alu_tb.f"
    )
)

$ErrorActionPreference = "Stop"

$ossCadEnv = Join-Path $env:USERPROFILE "scoop/apps/oss-cad-suite-nightly/current/environment.ps1"
if (-not (Test-Path $ossCadEnv)) {
    throw "OSS CAD Suite environment script not found: $ossCadEnv"
}

& $ossCadEnv | Out-Null
New-Item -ItemType Directory -Force -Path "build/hdl" | Out-Null

foreach ($fileList in $FileLists) {
    $name = [IO.Path]::GetFileNameWithoutExtension($fileList)
    $out = "build/hdl/$name.vvp"

    & iverilog -g2012 -o $out -f $fileList
    if ($LASTEXITCODE -ne 0) {
        throw "iverilog failed for $fileList"
    }

    & vvp $out
    if ($LASTEXITCODE -ne 0) {
        throw "vvp failed for $fileList"
    }
}
