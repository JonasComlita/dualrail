# TC-V31-PILOT-025: Service lifecycle handoff

Restart recovery restores process state but duplicates one device lease and loses the original failure cause.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-10, 10], subtract 0 from negative values, add 0 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 2; zero events add 2; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -1, zero returns it, positive mode multiplies it by 3 and adds -1.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent ts contract must advertise ABI level 25. Do not edit read-only fixtures or add files. At least one Trit file must change.
