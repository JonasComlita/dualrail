## SYCL Geometry Tuning Results
- **Device:** `AMD Ryzen 9 3900X 12-Core Processor            `
- **Backend Status:** CPU OpenCL/default SYCL path. AMD GPU SYCL via Codeplay AMD plugin has not been installed or validated; Codeplay's AMD plugin path targets Linux/ROCm rather than this Windows host.
- **Payload Count:** `10000000` lanes
- **Operation:** `addLane64 (20 trits)`

| Local Size | Execution Time (us) |
| ---------- | ------------------- |
| Auto | 841258 |
| 32 | 1718362 |
| 64 | 1697517 |
| 128 | 1697609 |
| 256 | 1691751 |
| 512 | 1696047 |
| 1024 | 1699355 |
