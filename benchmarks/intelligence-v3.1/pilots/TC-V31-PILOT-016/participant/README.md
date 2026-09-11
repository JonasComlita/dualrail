# TC-V31-PILOT-016: ABA-safe queue handoff

A bounded queue passes sequential tests but loses a slot when two claims wrap through the neutral generation.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-8, 8], subtract 0 from negative values, add 0 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 3; zero events add 3; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -2, zero returns it, positive mode multiplies it by 2 and adds -2.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent cpp contract must advertise ABI level 16. Do not edit read-only fixtures or add files. At least one Trit file must change.
