# TreatCode Platform Plan Index

Status values are `not_started`, `in_progress`, `blocked`, `complete`, or
`superseded`. `Complete` is valid only under
[COMPLETION_PROTOCOL.md](COMPLETION_PROTOCOL.md).

| ID | Plan | Depends on | Status | Completion evidence |
|---|---|---|---|---|
| P00 | [Plan control and verification](plans/P00_plan_control.md) | None | complete | `build/treatcode-plan-evidence/P00/result.json` |
| P01 | [Platform contracts and domain schemas](plans/P01_platform_contracts.md) | P00 | complete | `build/treatcode-plan-evidence/P01/result.json` |
| P02 | [Stack, capability, and decision registry](plans/P02_stack_capabilities_decisions.md) | P01 | complete | `build/treatcode-plan-evidence/P02/result.json` |
| P03 | [Repository ingestion and code intelligence](plans/P03_repository_ingestion.md) | P01, P02 | in_progress | machine reports under build/treatcode-plan-evidence/P03/ |
| P04 | [Public API and Stack Explorer](plans/P04_public_api_stack_explorer.md) | P02, P03 | in_progress | — |
| P05 | [Learning and documentation](plans/P05_learning_documentation.md) | P03, P04 | in_progress | machine reports under `build/treatcode-plan-evidence/P05/` |
| P06 | [Challenges and faceted taxonomy](plans/P06_challenges_taxonomy.md) | P01, P04 | in_progress | machine reports under `build/treatcode-plan-evidence/P06/` |
| P07 | [Identity, permissions, and agent access](plans/P07_identity_permissions_agents.md) | P01, P04 | in_progress | machine reports under `build/treatcode-plan-evidence/P07/` |
| P08 | [Remote development workspaces](plans/P08_remote_workspaces.md) | P03, P07 | in_progress | `build/treatcode-plan-evidence/P08/` |
| P09 | [Secure execution and immutable evidence](plans/P09_secure_execution_evidence.md) | P07, P08 | in_progress | `build/treatcode-plan-evidence/P09/result.json` |
| P10 | [Optimization and benchmark lab](plans/P10_optimization_benchmarks.md) | P06, P09 | in_progress | `build/treatcode-plan-evidence/P10/result.json` |
| P11 | [Uploads, contributions, and GitHub](plans/P11_uploads_contributions_github.md) | P07, P08, P09 | in_progress | `build/treatcode-plan-evidence/P11/result.json` |
| P12 | [Operations and mobile collaboration](plans/P12_operations_mobile.md) | P08, P09, P11 | in_progress | — |
| P13 | [Launch closure](plans/P13_launch_closure.md) | P04–P12 | in_progress | `build/treatcode-plan-evidence/P13/result.json` |

## Parallel Work

After P01:

- P02 must precede P03.
- P06 and P07 may run in parallel after P04.
- P05 may run in parallel with P07 after P03 and P04.
- P08 follows P07; P09 follows P08.
- P10 and P11 may run in parallel after P09.

## Change Control

- A dependency change requires updating this index and affected plan files.
- A plan may be split when its acceptance gates are preserved across the new
  plans and dependency edges.
- A completed plan may only be changed by a new amendment plan or by marking it
  `superseded` with a replacement.
