# TC-V31-PILOT-014: Arena checkpoint rollback

A failed nested allocation rewinds capacity without restoring generation metadata and exposes a stale handle.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-6, 6], subtract 1 from negative values, add 1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 6; zero events add 6; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus 0, zero returns it, positive mode multiplies it by 2 and adds 0.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent txt contract must advertise ABI level 14. Do not edit read-only fixtures or add files. At least one Trit file must change.
