param(
    [string[]] $Files = @(
        "hdl/rtl/trit_pkg.sv",
        "hdl/rtl/trit_gates.sv",
        "hdl/rtl/trit_lane.sv",
        "hdl/rtl/trit_lane_alu.sv",
        "hdl/tb/trit_gate_tb.sv",
        "hdl/tb/trit_lane_alu_tb.sv"
    )
)

$ErrorActionPreference = "Stop"

& verible-verilog-syntax @Files
if ($LASTEXITCODE -ne 0) {
    throw "verible-verilog-syntax failed"
}

& verible-verilog-lint --rules=-line-length,-parameter-name-style,-module-filename @Files
if ($LASTEXITCODE -ne 0) {
    throw "verible-verilog-lint failed"
}

$ossCadEnv = Join-Path $env:USERPROFILE "scoop/apps/oss-cad-suite-nightly/current/environment.ps1"
if (Test-Path $ossCadEnv) {
    & $ossCadEnv | Out-Null
    $env:VERILATOR_ROOT = Join-Path $env:YOSYSHQ_ROOT "share/verilator"

    & verilator_bin --lint-only --timing -f "hdl/trit_gate_tb.f" --top-module trit_gate_tb
    if ($LASTEXITCODE -ne 0) {
        throw "verilator lint failed for trit_gate_tb"
    }

    & verilator_bin --lint-only --timing -f "hdl/trit_lane_alu_tb.f" --top-module trit_lane_alu_tb
    if ($LASTEXITCODE -ne 0) {
        throw "verilator lint failed for trit_lane_alu_tb"
    }
}
