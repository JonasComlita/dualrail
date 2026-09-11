# TC-V31-PILOT-026: Incremental dependency rebuild

A header-only Trit ABI change skips the native wrapper target unless a clean build is performed.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-11, 11], subtract 1 from negative values, add 1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 3; zero events add 3; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus 0, zero returns it, positive mode multiplies it by 2 and adds 0.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent txt contract must advertise ABI level 26. Do not edit read-only fixtures or add files. At least one Trit file must change.
