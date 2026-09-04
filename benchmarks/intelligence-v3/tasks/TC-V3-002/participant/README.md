# Overflow-safe interval merge

Two closed intervals have endpoints in `[-40, 40]`. An interval `[start, end]`
is packed as `(start + 40) * 81 + (end + 40)`.

Repair `merge_touching(left, right)`. It must validate both intervals, normalize
their order, and merge intervals that overlap or are exactly adjacent. Return the
merged packed interval, `-1` when the valid intervals are disjoint, or `-2` when
either interval has `start > end`.

Do not change the public entrypoint or its two-argument signature. Avoid endpoint
successor arithmetic at the upper boundary; the hidden grader includes reversed,
adjacent, invalid, and boundary cases.
