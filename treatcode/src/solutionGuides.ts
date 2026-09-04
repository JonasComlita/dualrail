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
