# Stable three-way partition

The packed four-item partitioner sometimes changes the relative order of equal-side values. Diagnose the repository and repair the implementation.

Values are in `[-3, 3]`. Four values are packed from first to last as base-7 digits after adding 3. `stable_partition4(packed, pivot)` must place values below the pivot first, then values equal to it, then values above it, preserving original order within every group. The returned value uses the same packing.

Do not change the public entrypoint or its two-argument signature. The grader also enforces the documented input range and a linear operation budget.
