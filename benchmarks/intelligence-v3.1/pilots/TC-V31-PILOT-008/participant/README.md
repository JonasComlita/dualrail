# TC-V31-PILOT-008: Recovering precedence parser

Malformed nested ternary expressions shift associativity after recovery and poison the following declaration.

Repair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:

- `normalize_value(x)`: clamp to [-7, 7], subtract 1 from negative values, add 1 to positive values, and keep zero canonical.
- `transition_state(state,event,aux)`: negative events subtract aux and 5; zero events add 5; positive events add aux; normalize the result.
- `finalize_result(value,mode)`: normalize first; negative mode returns the negated value plus -2, zero returns it, positive mode multiplies it by 2 and adds -2.
- `bounded_fold(seed,steps)`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly `steps` transitions.

The adjacent cpp contract must advertise ABI level 8. Do not edit read-only fixtures or add files. At least one Trit file must change.
