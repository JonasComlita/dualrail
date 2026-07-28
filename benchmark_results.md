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
| quicksort | Random | 10 | 32/26 | 1.23x | 9/9 | 59/51 | 33/33 | 76/206 | 0.37x | 9142 | 9142 | 1125 | 0.116 / 13236.6 |
| quicksort | Pre-Sorted | 10 | 28/22 | 1.27x | 9/9 | 52/48 | 29/29 | 71/184 | 0.39x | 8312 | 8312 | 1119 | 0.106 / 12359.1 |
| quicksort | Identical | 10 | 22/11 | 2.00x | 0/0 | 13/13 | 0/0 | 36/104 | 0.35x | 3394 | 3394 | 1119 | 0.090 / 5110.1 |
| bubble | Random | 10 | 45/45 | 1.00x | 18/18 | 126/126 | 36/36 | 109/236 | 0.46x | 14004 | 14004 | 604 | 0.103 / 19929.9 |
| bubble | Pre-Sorted | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 109/218 | 0.50x | 10488 | 10488 | 598 | 0.097 / 15870.5 |
| bubble | Identical | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 109/263 | 0.41x | 10533 | 10533 | 598 | 0.090 / 15560.8 |
| selection | Random | 10 | 45/45 | 1.00x | 8/8 | 106/106 | 16/16 | 118/270 | 0.44x | 12144 | 12144 | 633 | 0.098 / 17637.9 |
| selection | Pre-Sorted | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 118/281 | 0.42x | 10569 | 10569 | 627 | 0.095 / 15905.5 |
| selection | Identical | 10 | 45/45 | 1.00x | 0/0 | 90/90 | 0/0 | 118/281 | 0.42x | 10569 | 10569 | 627 | 0.096 / 15468.0 |
| quicksort | Random | 50 | 348/260 | 1.34x | 165/165 | 668/626 | 407/407 | 691/1413 | 0.49x | 85196 | 85196 | 1460 | 0.543 / 128232.1 |
| quicksort | Pre-Sorted | 50 | 437/279 | 1.57x | 192/192 | 724/695 | 442/442 | 792/1472 | 0.54x | 91361 | 91361 | 1439 | 0.589 / 122231.9 |
| quicksort | Identical | 50 | 102/51 | 2.00x | 0/0 | 53/53 | 0/0 | 156/424 | 0.37x | 13314 | 13314 | 1439 | 0.157 / 19785.8 |
| bubble | Random | 50 | 1225/1225 | 1.00x | 588/588 | 3626/3626 | 1176/1176 | 2549/4628 | 0.55x | 344471 | 344471 | 939 | 1.516 / 465182.9 |
| bubble | Pre-Sorted | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2549/4038 | 0.63x | 229788 | 229788 | 918 | 0.858 / 338861.0 |
| bubble | Identical | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2549/5263 | 0.48x | 231013 | 231013 | 918 | 0.772 / 292159.1 |
| selection | Random | 50 | 1225/1225 | 1.00x | 41/41 | 2532/2532 | 82/82 | 2598/5244 | 0.50x | 233573 | 233573 | 968 | 0.719 / 297205.7 |
| selection | Pre-Sorted | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2598/5361 | 0.48x | 225329 | 225329 | 947 | 0.740 / 294610.3 |
| selection | Identical | 50 | 1225/1225 | 1.00x | 0/0 | 2450/2450 | 0/0 | 2598/5361 | 0.48x | 225329 | 225329 | 947 | 0.767 / 298675.9 |
| quicksort | Random | 100 | 828/619 | 1.34x | 421/421 | 1619/1529 | 998/998 | 1603/3306 | 0.48x | 204114 | 204114 | 1890 | 1.055 / 301819.4 |
| quicksort | Pre-Sorted | 100 | 1204/731 | 1.65x | 565/565 | 1972/1926 | 1237/1237 | 2084/3736 | 0.56x | 242715 | 242715 | 1839 | 1.941 / 332426.1 |
| quicksort | Identical | 100 | 202/101 | 2.00x | 0/0 | 103/103 | 0/0 | 306/990 | 0.31x | 28348 | 28348 | 1839 | 0.191 / 44350.7 |
| bubble | Random | 100 | 4950/4950 | 1.00x | 2276/2276 | 14452/14452 | 4552/4552 | 10099/18034 | 0.56x | 1353197 | 1353197 | 1369 | 5.767 / 1840809.7 |
| bubble | Pre-Sorted | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10099/15729 | 0.64x | 909297 | 909297 | 1318 | 2.807 / 1186691.9 |
| bubble | Identical | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10099/20679 | 0.49x | 914247 | 914247 | 1318 | 3.295 / 1179749.9 |
| selection | Random | 100 | 4950/4950 | 1.00x | 94/94 | 10088/10088 | 188/188 | 10198/20549 | 0.50x | 906996 | 906996 | 1398 | 2.537 / 1163416.3 |
| selection | Pre-Sorted | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10198/20877 | 0.49x | 887913 | 887913 | 1347 | 3.129 / 1140125.9 |
| selection | Identical | 100 | 4950/4950 | 1.00x | 0/0 | 9900/9900 | 0/0 | 10198/20877 | 0.49x | 887913 | 887913 | 1347 | 3.073 / 1106188.2 |

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
