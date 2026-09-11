# TC-V31-PILOT-021: Capability handle narrowing

A valid high-generation handle is truncated by the SDK wrapper and collides with a revoked entry.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-6, 6], subtract -1 from negative values, add -1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 3; zero events add 3; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -1, zero returns it, positive mode multiplies it by 3 and adds -1.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent cpp contract must advertise ABI level 21. Do not edit read-only fixtures or add files. At least one Trit file must change.
