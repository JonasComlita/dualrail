# TC-V31-PILOT-022: Configuration precedence repair

Environment, manifest, and runtime overrides produce different effective limits in the CLI and service paths.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-7, 7], subtract 0 from negative values, add 0 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 4; zero events add 4; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus 0, zero returns it, positive mode multiplies it by 2 and adds 0.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent ts contract must advertise ABI level 22. Do not edit read-only fixtures or add files. At least one Trit file must change.
