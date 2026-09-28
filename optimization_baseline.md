## Phase 5D Tiny Transformer VM Runtime Fixture

| Runtime | Shape | Assembly Words | VM Steps | Kernel Runs | DMEM Words | DMEM Loads | DMEM Stores | Runtime (us) | Checksum | Max Error | Notes |
| ------- | ----- | -------------: | -------: | ----------: | ---------: | ---------: | ----------: | -----------: | -------- | --------: | ----- |
| Tiny character transformer | vocab=4, seq=2, width=3, heads=1 | 1782 | 1782 | 6 | 138 | 156 | 48 | 13502 | 0xd317ad9abb2cdb68 | 4.65543e-10 | IR-generated VM kernels, no transformer opcodes |
