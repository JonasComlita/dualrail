# find_mul.py - BFS to find ternary multiplication using carryless/lattice ops

import collections

# Define the input domain: 9 pairs of (a, b) in {-1, 0, 1}^2
inputs = [
    (a, b) for a in [-1, 0, 1] for b in [-1, 0, 1]
]

# We want to find a sequence of ops to get target: a * b
target = tuple(a * b for (a, b) in inputs)

# Operations
def op_neg(v):
    return tuple(-x for x in v)

def op_add(v1, v2):
    res = []
    for x, y in zip(v1, v2):
        s = x + y
        while s > 1: s -= 3
        while s < -1: s += 3
        res.append(s)
    return tuple(res)

def op_sub(v1, v2):
    res = []
    for x, y in zip(v1, v2):
        s = x - y
        while s > 1: s -= 3
        while s < -1: s += 3
        res.append(s)
    return tuple(res)

def op_and(v1, v2):
    return tuple(min(x, y) for x, y in zip(v1, v2))

def op_or(v1, v2):
    return tuple(max(x, y) for x, y in zip(v1, v2))

# Inputs as starting vectors
v_a = tuple(a for (a, b) in inputs)
v_b = tuple(b for (a, b) in inputs)

# BFS
# Queue: (vector, expression, depth)
# We also keep a list of known vectors and their shortest expressions
known = {}
known[v_a] = "a"
known[v_b] = "b"

queue = collections.deque([v_a, v_b])

print("Target is:", target)

found = False
depth = 0

while queue and not found:
    size = len(queue)
    print(f"Level {depth}, size {len(known)}")
    for _ in range(size):
        curr = queue.popleft()
        curr_expr = known[curr]
        
        # Unary Neg
        v_neg = op_neg(curr)
        if v_neg not in known:
            known[v_neg] = f"neg({curr_expr})"
            queue.append(v_neg)
            if v_neg == target:
                print("FOUND:", known[v_neg])
                found = True
                break
        
        # Binary ops with all known
        for other in list(known.keys()):
            other_expr = known[other]
            
            # add
            v_add = op_add(curr, other)
            if v_add not in known:
                known[v_add] = f"add({curr_expr}, {other_expr})"
                queue.append(v_add)
                if v_add == target:
                    print("FOUND:", known[v_add])
                    found = True
                    break
            
            # sub
            v_sub = op_sub(curr, other)
            if v_sub not in known:
                known[v_sub] = f"sub({curr_expr}, {other_expr})"
                queue.append(v_sub)
                if v_sub == target:
                    print("FOUND:", known[v_sub])
                    found = True
                    break
            
            # and
            v_and = op_and(curr, other)
            if v_and not in known:
                known[v_and] = f"and({curr_expr}, {other_expr})"
                queue.append(v_and)
                if v_and == target:
                    print("FOUND:", known[v_and])
                    found = True
                    break
            
            # or
            v_or = op_or(curr, other)
            if v_or not in known:
                known[v_or] = f"or({curr_expr}, {other_expr})"
                queue.append(v_or)
                if v_or == target:
                    print("FOUND:", known[v_or])
                    found = True
                    break
        if found:
            break
    depth += 1
    if depth > 8:
        print("Depth limit reached")
        break
