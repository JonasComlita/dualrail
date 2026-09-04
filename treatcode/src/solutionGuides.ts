export type SolutionGuide = {
  plainEnglish: string;
  pseudocode: string;
  discussion: string;
};

/**
 * Editorial learning notes are deliberately separate from participant source.
 * They explain the contract without exposing private saved solutions or hidden
 * verifier fixtures.
 */
export const PRACTICE_SOLUTION_GUIDES: Record<string, SolutionGuide> = {
  T001: {
    plainEnglish: "Compare the input with zero. A negative value maps to -1, an equal value maps to 0, and a positive value maps to +1. The three outcomes line up directly with TCL's three-arm match.",
    pseudocode: `compare x with 0
match the comparison:
  negative -> return -1
  zero     -> return 0
  positive -> return +1`,
    discussion: "The important idea is to preserve all three states instead of collapsing the result to a Boolean. The match arms are the sign function's complete partition of the input space, so no extra case or conditional is needed.",
  },
  T002: {
    plainEnglish: "Walk from 1 through N. For each number, test whether division by three leaves no remainder. Print Trit for those numbers; otherwise print the number itself. Keep the loop counter moving forward exactly once per output line.",
    pseudocode: `i = 1
while i <= N:
  if i is divisible by 3:
    print "Trit" and a newline
  else:
    print i and a newline
  i = i + 1`,
    discussion: "The loop owns the range and the output branch owns only the current number. That separation makes the boundary cases N = 1 and N = 50 behave the same way as the middle of the range.",
  },
  T005: {
    plainEnglish: "First find the final character by walking until the null terminator. Then keep one pointer at the front and one at the back, swap those characters, and move both pointers inward until they meet.",
    pseudocode: `right = index of the null terminator - 1
left = 0
while left < right:
  swap buffer[left] and buffer[right]
  left = left + 1
  right = right - 1
leave the null terminator in place`,
    discussion: "The buffer is reversed in place, so the algorithm uses constant extra storage and preserves the validated pointer contract. The null terminator is a boundary marker, not part of the text, which is why it must not be swapped.",
  },
  T056: {
    plainEnglish: "Use the two base cases directly: Fibonacci zero is zero and Fibonacci one is one. Every larger value is the sum of the two immediately preceding values, so the recursive function calls itself twice and adds the results.",
    pseudocode: `fibonacci(n):
  if n == 0: return 0
  if n == 1: return 1
  return fibonacci(n - 1) + fibonacci(n - 2)`,
    discussion: "This is intentionally not the fastest Fibonacci algorithm. The challenge is testing recursive calls, return values, and register spilling in the VM, so the straightforward recurrence is the behavior being evaluated.",
  },
  T057: {
    plainEnglish: "Treat the word as packed trit lanes rather than as one ordinary integer. Apply the requested mask operation lane by lane, then return the packed result using the same representation.",
    pseudocode: `for each trit lane in the packed word:
  result[lane] = tritwise mask(input[lane])
return the packed result`,
    discussion: "The result can match a scalar reference while the cost differs by representation. Keeping the operation tritwise is therefore part of correctness: unpacking to a scalar and rebuilding the word may violate the published word-parallel budget.",
  },
  T058: {
    plainEnglish: "Multiply each matching pair of lanes and add the three products. The lane order is explicit, so a0 pairs with b0, a1 with b1, and a2 with b2.",
    pseudocode: `product0 = a0 * b0
product1 = a1 * b1
product2 = a2 * b2
return product0 + product1 + product2`,
    discussion: "The challenge is a small, deterministic vector contract. Keeping the pairings explicit makes it easy to check and avoids accidentally mixing lanes or treating the vector as a single encoded scalar.",
  },
};

export const INTELLIGENCE_SOLUTION_GUIDES: Record<string, SolutionGuide> = {
  "TC-SWE-001": {
    plainEnglish: "Validate every reading before choosing a result. Ignore the missing sentinel after validation, then handle zero, one, two, or three remaining readings in that order. Two readings are averaged toward zero; three readings use the median.",
    pseudocode: `if any reading is invalid: return -1001
valid = readings excluding -1000
if valid is empty: return -1000
if valid has one value: return that value
if valid has two values: return (valid[0] + valid[1]) / 2 toward zero
return median(valid[0], valid[1], valid[2])`,
    discussion: "Validation precedence is the main invariant: an invalid reading must not be mistaken for a missing one. Keeping validation, filtering, and aggregation as separate steps also makes the signed-rounding and median helpers independently testable.",
  },
  "TC-SWE-002": {
    plainEnglish: "Parse the token into its checked parts, reject malformed digits or a bad checksum before decoding, then decode the balanced-ternary payload. Apply the requested rotation and serialize the checked result in the required format.",
    pseudocode: `parse token fields
validate field shape, digit alphabet, and checksum
if validation fails: return the specified error
decode balanced-ternary digits
rotate the decoded trits
encode the result and emit a fresh checksum`,
    discussion: "The order of checks matters more than clever decoding. A parser that decodes first can turn malformed input into a plausible value, while a serializer that forgets to recompute the checksum can produce an output that cannot survive the next validation pass.",
  },
  "TC-SWE-003": {
    plainEnglish: "Treat a span as half-open: it includes start and excludes end. Check pointer ownership, arena bounds, alignment, and range order before converting a valid span into the requested result.",
    pseudocode: `validate pointer and supported alignment
validate start >= arena_start and end <= arena_end
validate start <= end
if any check fails: return the required error by precedence
return the validated span length or inclusive result`,
    discussion: "Half-open ranges make adjacent spans compose without overlap. The safety contract is enforced before arithmetic is trusted, so an invalid alignment or out-of-range pointer cannot be hidden by a later conversion.",
  },
  "TC-SWE-004": {
    plainEnglish: "Validate both events, start with the supplied state, apply the first event, and only then apply the second. Each transition handles reset, sign toggling, and saturation according to the event contract.",
    pseudocode: `validate state and both events
state = apply(state, first_event)
state = apply(state, second_event)
clamp state at the permitted bounds
return state`,
    discussion: "This task is about ordered state transitions, not just the final set of events. Swapping the events can change a reset, toggle, or saturation result, so the implementation must preserve the input order and the documented error precedence.",
  },
  "TC-SWE-005": {
    plainEnglish: "Normalize the kernel response before applying the caller's policy. A negative kernel status is authoritative; otherwise pass the partial result through the selected clamp or Boolean policy and return the bounded value.",
    pseudocode: `read kernel status and result
if status is negative: return the normalized kernel error
validate the caller policy
apply the policy to the result
clamp to the allowed return range
return the adapted value`,
    discussion: "The adapter is an ABI boundary, so it must not reinterpret an error as a successful result. Keeping status handling ahead of policy transformation preserves the kernel contract while still giving callers a predictable bounded value.",
  },
};
