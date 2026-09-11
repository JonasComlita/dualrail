# TC-V31-PILOT-004: Weighted path tie contract

A blocked-grid path solver finds the right distance but violates lexicographic predecessor selection across equal-cost routes.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-10, 10], subtract 0 from negative values, add 0 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 6; zero events add 6; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -2, zero returns it, positive mode multiplies it by 2 and adds -2.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent txt contract must advertise ABI level 4. Do not edit read-only fixtures or add files. At least one Trit file must change.
