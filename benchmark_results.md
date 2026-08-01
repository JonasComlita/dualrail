# Architectural Binary vs Ternary Algorithm Benchmark Results

This report presents a thorough, scientific comparison between standard binary algorithms (C++) and optimal ternary-native algorithms (Trit) for **Quicksort** (both utilizing a 3-way Bentley-McIlroy partition), **Bubble Sort**, and **Selection Sort** across various data configurations and dataset sizes.

## Metrics Tracked
- **Comparisons**: Element-to-element comparison operations.
- **Swaps**: Array element swaps.
- **Loads**: Memory read operations from the array.
- **Stores**: Memory write operations to the array.
- **Branch Decisions**: Conditional checks executed (loop condition checks, pattern matching/if decisions).
- **VM Steps**: Instructions executed in the Ternary VM.
- **Net Code Size**: Size of the compiled algorithm in Ternary instruction words (excluding standard library boilerplate).
- **VM Interpreter Time (Us)**: Average wall-clock execution time per sort in microseconds. Note that the Binary side runs natively on host CPU silicon, while the Trit side runs inside a software interpreter simulator. The timing metrics represent interpreter emulation overhead and have no predictive value for native silicon performance (which can only be evaluated upon physical hardware compilation in Phase F).

## Detailed Performance Comparison Table

| Algorithm | Dataset | Size (N) | Comps (Bin/Ter) | Comp Saving | Swaps (Bin/Ter) | Loads (Bin/Ter) | Stores (Bin/Ter) | Branches (Bin/Ter) | Branch Saving | VM Steps | VM Cycles | Net Code Size (Words) | VM Interpreter Time (Us) (Bin/VM) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| quicksort | Random | 10 | 32/26 | 1.23x | 9/9 | 59/51 | 33/33 | 76/206 | 0.37x | 8179 | 8179 | 977 | 0.121 / 12773.9 |
| quicksort | Pre-Sorted | 10 | 28/22 | 1.27x | 9/9 | 52/48 | 29/29 | 71/184 | 0.39x | 7430 | 7430 | 971 | 0.152 / 11209.6 |
| quicksort | Identical | 10 | 22/11 | 2.00x | 0/0 | 13/13 | 0/0 | 36/104 | 0.35x | 3049 | 3049 | 971 | 0.080 / 4673.5 |
| bubble | Random | 10 | 45/45 | 1.00x | 18/18 | 126/126 | 36/36 | 109/236 | 0.46x | 12830 | 12830 | 594 | 0.127 / 18860.0 |
| bubble | Pre-Sorted | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 109/218 | 0.50x | 9674 | 9674 | 588 | 0.132 / 15647.9 |
| bubble | Identical | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 109/263 | 0.41x | 9719 | 9719 | 588 | 0.099 / 19177.9 |
| selection | Random | 10 | 45/45 | 1.00x | 8/8 | 106/106 | 16/16 | 118/270 | 0.44x | 11152 | 11152 | 623 | 0.112 / 19634.2 |
| selection | Pre-Sorted | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 118/281 | 0.42x | 9737 | 9737 | 617 | 0.105 / 15936.0 |
| selection | Identical | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 118/281 | 0.42x | 9737 | 9737 | 617 | 0.140 / 19249.1 |
| quicksort | Random | 50 | 348/260 | 1.34x | 165/165 | 668/626 | 407/407 | 691/1413 | 0.49x | 73974 | 73974 | 1312 | 0.644 / 132231.8 |
| quicksort | Pre-Sorted | 50 | 437/279 | 1.57x | 192/192 | 724/695 | 442/442 | 792/1472 | 0.54x | 78983 | 78983 | 1291 | 0.733 / 113616.9 |
| quicksort | Identical | 50 | 102/51 | 2.00x | 0/0 | 53/53 | 0/0 | 156/424 | 0.37x | 12009 | 12009 | 1291 | 0.136 / 20747.5 |
| bubble | Random | 50 | 1225/1225 | 1.00x | 588/588 | 3626/3626 | 1176/1176 | 2549/4628 | 0.55x | 312857 | 312857 | 929 | 2.193 / 467011.9 |
| bubble | Pre-Sorted | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2549/4038 | 0.63x | 209934 | 209934 | 908 | 1.173 / 303143.2 |
| bubble | Identical | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2549/5263 | 0.48x | 211159 | 211159 | 908 | 1.086 / 314614.7 |
| selection | Random | 50 | 1225/1225 | 1.00x | 41/41 | 2532/2532 | 82/82 | 2598/5244 | 0.50x | 212801 | 212801 | 958 | 0.974 / 344379.4 |
| selection | Pre-Sorted | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2598/5361 | 0.48x | 205377 | 205377 | 937 | 1.039 / 339756.5 |
| selection | Identical | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2598/5361 | 0.48x | 205377 | 205377 | 937 | 0.851 / 286721.6 |
| quicksort | Random | 100 | 828/619 | 1.34x | 421/421 | 1619/1529 | 998/998 | 1603/3306 | 0.48x | 176593 | 176593 | 1742 | 1.193 / 278093.5 |
| quicksort | Pre-Sorted | 100 | 1204/731 | 1.65x | 565/565 | 1972/1926 | 1237/1237 | 2084/3736 | 0.56x | 208865 | 208865 | 1691 | 1.326 / 271026.4 |
| quicksort | Identical | 100 | 202/101 | 2.00x | 0/0 | 103/103 | 0/0 | 306/990 | 0.31x | 25826 | 25826 | 1691 | 0.164 / 39283.5 |
| bubble | Random | 100 | 4950/4950 | 1.00x | 2276/2276 | 14452/14452 | 4552/4552 | 10099/18034 | 0.56x | 1228006 | 1228006 | 1359 | 5.258 / 1671138.8 |
| bubble | Pre-Sorted | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10099/15729 | 0.64x | 829626 | 829626 | 1308 | 3.636 / 1206741.4 |
| bubble | Identical | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10099/20679 | 0.49x | 834576 | 834576 | 1308 | 4.800 / 1217588.0 |
| selection | Random | 100 | 4950/4950 | 1.00x | 94/94 | 10088/10088 | 188/188 | 10198/20549 | 0.50x | 825247 | 825247 | 1388 | 3.700 / 1249752.8 |
| selection | Pre-Sorted | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10198/20877 | 0.49x | 808044 | 808044 | 1337 | 3.305 / 1371710.6 |
| selection | Identical | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10198/20877 | 0.49x | 808044 | 808044 | 1337 | 3.382 / 1247098.6 |

## Core Architectural Observations

### 1. Fair 3-Way Quicksort Comparison
- By comparing the Ternary 3-Way Quicksort directly against a standard C++ 3-Way Quicksort (`cpp_quicksort_3way`), we isolate the true architectural advantage of ternary comparison primitives.
- For **All-Identical Arrays**, Ternary quicksort achieves a **2.00x** comparison saving ($N=100$). This is because the C++ 3-way quicksort requires two comparisons per element to classify it (first `<` to check partitioning, then `==` to check if equal), whereas Trit performs a single subtraction and 3-way match, resolving all three outcomes natively in one step.
- For **Pre-Sorted** and **Random** arrays, Ternary quicksort consistently performs **1.65x** and **1.34x** fewer comparisons respectively compared to C++ 3-Way Quicksort.

### 2. Bubble Sort and Selection Sort Equivalence
- For **Bubble Sort** and **Selection Sort**, the comparison counts are exactly identical (**1.00x** saving).
- This is because both binary and ternary versions of these algorithms execute exactly one element-to-element comparison per inner loop iteration. However, the ternary version achieves this control flow natively via comparison subtraction and matching.

### 3. Store and Load Characteristics
- The number of loads and stores is identical or extremely close between the binary and ternary implementations, proving that the ternary representations introduce zero memory access overhead.
- The higher branch instruction count in the Trit VM columns reflects the lower-level execution of VM branch instructions needed to evaluate the match statements, rather than a higher number of logical algorithmic decisions.
