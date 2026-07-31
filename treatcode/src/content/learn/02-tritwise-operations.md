---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "tritwise-operations",
  "title": "Tritwise operations",
  "module": "2. Operations",
  "level": "beginner",
  "order": 20,
  "summary": "Separate per-trit logic from numeric arithmetic and understand invalid lane states.",
  "prerequisites": ["representation-boundaries"],
  "sources": [
    { "path": "ternary_lanes.h", "label": "Lane operations and encoding", "kind": "source" },
    { "path": "ternary_native_ops.h", "label": "Numeric arithmetic operations", "kind": "source" },
    { "path": "ternary_simd.h", "label": "Vector operation surface", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane operation tests", "kind": "test" },
    { "path": "tests/test_native_ops.cpp", "label": "Numeric operation tests", "kind": "test" }
  ],
  "next": "first-tcl-program",
  "interactive": {
    "kind": "choice",
    "title": "Lane safety check",
    "prompt": "What should happen when a two-bit lane contains the pair 11?",
    "options": ["Treat it as +1", "Treat it as zero", "Reject it as invalid"],
    "answer": 2,
    "explanation": "The pair 11 is reserved as an invalid lane value so malformed transport data can be detected instead of silently becoming a number."
  }
}
---

## Two different kinds of operation

Numeric addition interprets a word as a number. A tritwise operation combines
corresponding trits and preserves the lane boundary. For example, `tladd.l20`
and `tland.l20` work on packed lanes, while `vadd.t40` performs numeric
arithmetic per vector lane.

Do not implement a lane operation with ordinary host `+`, `&`, or `|` on the
packed integer. Those operators can move carries or combine the two-bit
encoding instead of applying the specified trit truth table.

## A useful review question

When reading a function, ask: **does this code need the value of the number, or
does it need the state of each trit?** The answer chooses numeric conversion or
lane-preserving operations.

## Prerequisites

Complete **Representation boundaries** first. You should be able to explain
why `t20` and `l20` are not interchangeable even though both contain 20 trits.

## Production and evidence

The lane and native-operation headers define the behavior; the focused tests
exercise conversions, invalid patterns, and arithmetic separately. This page
does not copy those implementations into the website.

## Next step

Continue to **Your first TCL program**, where the same distinction appears in a
source language type rather than a C++ helper.
