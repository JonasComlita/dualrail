# Implementing Agent Advisory: `ulib/ternary_map.trit`

**Phase:** D5 (Relational State Store Initialization)
**Depends on:** Buffer pool manager (D3), WAL engine (D4), `ulib.trit` ownership signatures (A8)
**Do not begin until:** Phase B and C are complete and the native compiler is operational.

---

## Overview

You are implementing a ternary-native open-addressed hash map for use in the kernel's relational state store. It will back the Process Table, File Descriptor Table, and Quotas Table introduced in Phase D5. It will later be available to user-space programs via `ulib`.

The design uses `TCMP`-driven bidirectional probing to reduce clustering compared to linear probing. Read this advisory in full before writing any code. Several constraints here exist to prevent bugs that are easy to introduce and hard to diagnose.

---

## Constraint 1: Table Capacity Must Always Be a Power of 3

Table capacity must be constrained to values of the form `3^N` for some integer N.

This is not aesthetic. Bidirectional quadratic probing does not guarantee that every slot is reachable for arbitrary table sizes. For capacity = `3^N`, the probe sequence covers all slots before cycling, which means the table can be considered full only when all slots are genuinely occupied. For any other capacity, you will get infinite loops or false "table full" errors on tables that still have empty slots.

Resize always allocates at `3^(N+1)` — the next power of 3. There is no other valid resize target.

When the kernel calls `map_new` with a requested capacity that is not a power of 3, round up to the next power of 3 silently. Document this in the function header.

---

## Constraint 2: The Zero State Is Not Enough for Bucket Status

The original design described the `zero` state as covering both "tombstone" and "empty sentinel" bucket states. This will cause incorrect behaviour and must not be implemented that way.

A **tombstone** marks a slot where an entry was deleted. Probing must continue through a tombstone — a key inserted after the deleted entry may have probed through this slot and landed further along the chain.

An **empty sentinel** marks a slot that was never occupied. Probing stops at an empty sentinel — no entry can exist beyond it in the chain.

If both states map to `zero`, lookup has no way to distinguish them. A search that reaches a tombstone will stop early and report a false negative for keys that probed past it.

Use the 9-trit metadata field in the bucket word to encode status explicitly with one dedicated trit:

```
metadata trit 0: neg = tombstone | zero = empty | pos = occupied
```

The remaining 8 trits of the metadata field are available for tag fingerprinting (see Constraint 4).

Lookup logic must be:
- `occupied`: compare key, continue probing if mismatch
- `tombstone`: continue probing unconditionally, record slot as candidate for insertion
- `empty`: stop probing, key is not present

Insertion logic must:
- probe until `empty` or `tombstone`
- prefer the first `tombstone` slot encountered as the insertion target if no `empty` slot is found first

---

## Constraint 3: Two Independent Hash Functions Are Required

The probe direction is derived from a secondary hash of the key. This secondary hash must be computed independently of the primary index hash.

If you derive the probe direction from the primary hash — for example by taking its sign directly — keys that collide at the home bucket will also share the same probe direction, and the symmetry benefit is lost. You get asymmetric clustering in one direction, which is worse than standard linear probing in the degenerate case.

Use two separate hash computations:

```
primary_hash(key)   -> home bucket index H, range [0, capacity)
secondary_hash(key) -> probe direction sign, range {neg, zero, pos}
```

For the secondary hash, `zero` as a probe direction is degenerate — it means probe step is zero and the sequence never advances. Map `zero` outputs from the secondary hash to `pos` (or `neg` — pick one and be consistent). Document the choice.

The probe sequence from home bucket H with direction sign D and step i is:

```
neg:  probe_index(i) = (H - i*i) mod capacity
pos:  probe_index(i) = (H + i*i) mod capacity
```

Step i starts at 1 and increments each probe. i=0 is the home bucket itself.

---

## Constraint 4: The Vector Optimisation Is Conditional, Not Universal

The document describing this data structure claims that a 9-word burst loads 27 buckets simultaneously into the vector register file for parallel evaluation. This is true under a specific precondition that must be stated wherever this optimisation is used:

**The vector batch evaluation path applies only when keys and values both fit within the 9-trit sub-word constraint.**

If keys are variable-length strings, pointer-sized values, or structs larger than 9 trits, the bucket word layout does not pack cleanly and the vector path does not apply. In that case the map falls back to sequential per-bucket evaluation. This is not a failure — it is the correct behaviour for data that does not fit the sub-word constraint.

Implement two internal probe paths:

- `probe_vector`: used when key and value types are statically known to fit in 9 trits. Loads 9-word blocks and evaluates up to 27 buckets per vector instruction.
- `probe_scalar`: used otherwise. Evaluates one bucket per iteration.

The public API is identical for both paths. The selection between them is made at compile time via the width-parametric type system — a `map_lookup<W: TritWidth>` where W ≤ T9 takes the vector path; wider types take the scalar path. Do not make this a runtime branch.

The metadata tag fingerprint (the remaining 8 trits of the metadata field after the status trit) is used to filter false positives in the vector path without full key comparison. Store the low 8 trits of the secondary hash in these bits. During vector evaluation, compare fingerprints across all 27 buckets in parallel; only perform full key comparison on buckets where the fingerprint matches. This eliminates most false positive key comparisons without leaving the vector register file.

---

## Constraint 5: Load Factor and Resize Policy

Maximum load factor is **0.7**. When `occupied_count / capacity > 0.7`, the next insert triggers a resize before the insert proceeds.

Resize procedure:
1. Allocate a new backing array at capacity `3^(N+1)`.
2. Iterate the old array. For each `occupied` bucket, reinsert into the new array using the standard insert path. Skip `tombstone` and `empty` slots — tombstones are not carried over.
3. Free the old backing array.
4. Update the map header to point to the new array.

Not carrying tombstones forward is important. Tombstones accumulate over time and degrade probe chain performance. Resize is the natural point to eliminate them. A table that has had many deletions will see probe chain lengths drop significantly after resize even if occupied count is unchanged.

There is no shrink path in this implementation. If you need one later, add it as a separate function with explicit caller control — do not trigger shrink automatically on deletion.

---

## Struct Layout

```
struct TernaryMap {
    buckets:       ptr<T40, unknown>,  // backing array of bucket words
    capacity:      T40,                // always 3^N
    occupied:      T40,                // count of occupied slots (excludes tombstones)
    tombstones:    T40,                // count of tombstone slots
    key_width:     T40,                // trit width of key type
    val_width:     T40,                // trit width of value type
}

// Each bucket is one 27-trit word:
// [ 9-trit key fragment | 9-trit value | 9-trit metadata ]
// metadata: trit[0] = status {neg=tombstone, zero=empty, pos=occupied}
//           trit[1..8] = fingerprint (low 8 trits of secondary_hash(key))
```

---

## Public API

```
fn map_new(initial_capacity: T40) -> own<TernaryMap>
fn map_free(m: own<TernaryMap>) -> void
fn map_insert(m: borrow_mut<TernaryMap>, key: T40, val: T40) -> T1
fn map_lookup(m: borrow<TernaryMap>, key: T40, out: ptr<T40, unknown>) -> T1
fn map_delete(m: borrow_mut<TernaryMap>, key: T40) -> T1
fn map_len(m: borrow<TernaryMap>) -> T40
```

Return values: `(+1)` = success / found, `(-1)` = failure / not found, `0` = collision / table error.

`map_lookup` writes the found value into `out` if return is `(+1)`. `out` is unchanged on `(-1)`.

---

## What This Is Not

This map is not a general-purpose replacement for the Ternary Search Tree used in the compiler frontend (Phase B3). The TST is better suited to prefix-keyed string lookups and remains the correct structure for the type substitution map. Do not replace it with this.

This map is not thread-safe by itself. Concurrent access from multiple kernel threads requires the caller to hold the appropriate `shared<TernaryMap, ACQ_REL>` lock. Do not add internal locking — keep the data structure itself stateless with respect to concurrency and let the kernel's existing synchronisation primitives handle it.
