# TreatCode Platform Plan Index

Status values are `not_started`, `in_progress`, `blocked`, `complete`, or
`superseded`. `Complete` is valid only under
[COMPLETION_PROTOCOL.md](COMPLETION_PROTOCOL.md).

| ID | Plan | Depends on | Status | Completion evidence |
|---|---|---|---|---|
| P00 | [Plan control and verification](plans/P00_plan_control.md) | None | not_started | — |
| P01 | [Platform contracts and domain schemas](plans/P01_platform_contracts.md) | P00 | not_started | — |
| P02 | [Stack, capability, and decision registry](plans/P02_stack_capabilities_decisions.md) | P01 | not_started | — |
| P03 | [Repository ingestion and code intelligence](plans/P03_repository_ingestion.md) | P01, P02 | not_started | — |
| P04 | [Public API and Stack Explorer](plans/P04_public_api_stack_explorer.md) | P02, P03 | not_started | — |
| P05 | [Learning and documentation](plans/P05_learning_documentation.md) | P03, P04 | not_started | — |
| P06 | [Challenges and faceted taxonomy](plans/P06_challenges_taxonomy.md) | P01, P04 | not_started | — |
| P07 | [Identity, permissions, and agent access](plans/P07_identity_permissions_agents.md) | P01, P04 | not_started | — |
| P08 | [Remote development workspaces](plans/P08_remote_workspaces.md) | P03, P07 | not_started | — |
| P09 | [Secure execution and immutable evidence](plans/P09_secure_execution_evidence.md) | P07, P08 | not_started | — |
| P10 | [Optimization and benchmark lab](plans/P10_optimization_benchmarks.md) | P06, P09 | not_started | — |
| P11 | [Uploads, contributions, and GitHub](plans/P11_uploads_contributions_github.md) | P07, P08, P09 | not_started | — |
| P12 | [Operations and mobile collaboration](plans/P12_operations_mobile.md) | P08, P09, P11 | not_started | — |
| P13 | [Launch closure](plans/P13_launch_closure.md) | P04–P12 | not_started | — |

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

