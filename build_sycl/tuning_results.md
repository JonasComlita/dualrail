## SYCL Geometry Tuning Results
- **Device:** `AMD Ryzen 9 3900X 12-Core Processor            `
- **Payload Count:** `10000000` lanes
- **Operation:** `addLane64 (20 trits)`

| Local Size | Execution Time (us) |
| ---------- | ------------------- |
| Auto | 895109 |
| 32 | 1761061 |
| 64 | 2025556 |
| 128 | 1799758 |
| 256 | 1813226 |
| 512 | 1791342 |
| 1024 | 1726797 |

