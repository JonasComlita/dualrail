# TC-V31-PILOT-001: Deterministic cycle witness

A dependency planner returns a partial order instead of the canonical cycle witness after an incremental edge update.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-7, 7], subtract 0 from negative values, add 0 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 3; zero events add 3; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -1, zero returns it, positive mode multiplies it by 3 and adds -1.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent ts contract must advertise ABI level 1. Do not edit read-only fixtures or add files. At least one Trit file must change.
