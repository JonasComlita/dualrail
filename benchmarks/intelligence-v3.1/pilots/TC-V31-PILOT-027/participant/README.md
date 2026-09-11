# TC-V31-PILOT-027: Generated manifest consistency

The generator and validator canonicalize equivalent task data differently and produce unstable evidence hashes.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-12, 12], subtract -1 from negative values, add -1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 4; zero events add 4; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus 1, zero returns it, positive mode multiplies it by 3 and adds 1.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent ts contract must advertise ABI level 27. Do not edit read-only fixtures or add files. At least one Trit file must change.
