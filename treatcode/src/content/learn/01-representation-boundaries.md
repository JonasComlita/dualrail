---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "representation-boundaries",
  "title": "Representation boundaries",
  "module": "1. Representation",
  "level": "beginner",
  "order": 10,
  "summary": "Learn why numeric, lane, and hardware-facing trit representations are separate contracts.",
  "prerequisites": [],
  "sources": [
    { "path": "docs/00_Quick_Ref/trit_encoding.md", "label": "Trit encoding quick reference", "kind": "documentation" },
    { "path": "ternary_scalar.h", "label": "Positional numeric storage", "kind": "source" },
    { "path": "ternary_lanes.h", "label": "Packed lane storage", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane conversion regression tests", "kind": "test" },
    { "path": "tests/test_native_ops.cpp", "label": "Native numeric operation tests", "kind": "test" }
  ],
  "next": "tritwise-operations",
  "interactive": {
    "kind": "choice",
    "title": "Boundary check",
    "prompt": "Which representation is the numeric source of truth for scalar arithmetic?",
    "options": ["Two bits per trit", "Positional base-3 storage", "The HAL enum"],
    "answer": 1,
    "explanation": "Scalar arithmetic uses positional base-3 values. Packed lanes and enums are different interface or transport representations."
  }
}
---

## Why this comes first

A trit is a value with three possible numeric states: `-1`, `0`, and `+1`.
The project stores those states in binary host memory, but the host container is
not the meaning of the value. Keeping that boundary explicit prevents a lane
payload from being mistaken for an integer.

## The three representations

| Representation | What it is for | Important rule |
|---|---|---|
| Positional base-3 | Scalar numeric arithmetic | Decode each digit as `stored_digit - 1`. |
| Two-bit lane | SIMD, GPU, vector, and packed tritwise transport | `00=-1`, `01=0`, `10=+1`, and `11` is invalid. |
| Trit enum | Hardware-facing helper APIs | It names a state; it is not numeric storage. |

`T40` and `T50` are numeric widths. `l40` and `l50` have the same trit counts
but use lane encoding. The width name alone does not tell you which semantics
apply.

## Prerequisites

None. Start here if balanced ternary, lanes, or the VM are new to you.

## Production and evidence

Read the quick reference and then compare the scalar and lane headers. The
linked regression tests are the shortest executable proof that conversion is
intentional rather than a display convention.

## Next step

Continue to **Tritwise operations** and practice applying an operation to each
lane without doing scalar arithmetic on the packed payload.
