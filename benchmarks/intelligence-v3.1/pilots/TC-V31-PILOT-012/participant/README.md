# TC-V31-PILOT-012: Deterministic module linker

Module initialization order depends on filesystem enumeration and misresolves one weak symbol cycle.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-11, 11], subtract -1 from negative values, add -1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 4; zero events add 4; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -2, zero returns it, positive mode multiplies it by 2 and adds -2.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent cpp contract must advertise ABI level 12. Do not edit read-only fixtures or add files. At least one Trit file must change.
