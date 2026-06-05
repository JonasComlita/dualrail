# Trit HDL

This directory contains the first SystemVerilog RTL layer for the Trit dual-rail
transport contract.

## Encoding

Each trit is encoded as a two-bit pair:

| Trit | Pair |
| ---- | ---- |
| -1   | `2'b00` |
| 0    | `2'b01` |
| +1   | `2'b10` |
| invalid | `2'b11` |

The RTL follows the sticky-invalid rule for arithmetic and lane logic. TSEL is
parameterized with `POISON_UNUSED_ARMS`; the default copies the selected arm and
poisons invalid predicates or selected invalid data, matching the VM
tag-preserving select behavior.

## Files

- `rtl/trit_pkg.sv`: shared encoding constants and synthesizable helper functions.
- `rtl/trit_gates.sv`: single-trit gates, TSEL, and balanced ternary full adder.
- `rtl/trit_lane.sv`: parameterized packed-lane gates and ripple add/subtract.
- `tb/trit_gate_tb.sv`: self-checking unit testbench.

## Example Simulation Commands

With Verilator:

```sh
verilator --binary --timing -f hdl/trit_gate_tb.f
./obj_dir/Vtrit_gate_tb
```

With Icarus Verilog:

```sh
iverilog -g2012 -o build/trit_gate_tb.vvp -f hdl/trit_gate_tb.f
vvp build/trit_gate_tb.vvp
```
