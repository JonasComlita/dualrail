# TreatCode Platform Plan Index

Status values are `not_started`, `in_progress`, `blocked`, `complete`, or
`superseded`. `Complete` is valid only under
[COMPLETION_PROTOCOL.md](COMPLETION_PROTOCOL.md).

| ID | Plan | Depends on | Status | Completion evidence |
|---|---|---|---|---|
| P00 | [Plan control and verification](plans/P00_plan_control.md) | None | complete | `build/treatcode-plan-evidence/P00/result.json` |
| P01 | [Platform contracts and domain schemas](plans/P01_platform_contracts.md) | P00 | complete | `build/treatcode-plan-evidence/P01/result.json` |
| P02 | [Stack, capability, and decision registry](plans/P02_stack_capabilities_decisions.md) | P01 | complete | `build/treatcode-plan-evidence/P02/result.json` |
| P03 | [Repository ingestion and code intelligence](plans/P03_repository_ingestion.md) | P01, P02 | complete | `build/treatcode-plan-evidence/P03/result.json` |
| P04 | [Public API and Stack Explorer](plans/P04_public_api_stack_explorer.md) | P02, P03 | complete | `build/treatcode-plan-evidence/P04/result.json` |
| P05 | [Learning and documentation](plans/P05_learning_documentation.md) | P03, P04 | complete | `build/treatcode-plan-evidence/P05/result.json` |
| P06 | [Challenges and faceted taxonomy](plans/P06_challenges_taxonomy.md) | P01, P04 | complete | `build/treatcode-plan-evidence/P06/result.json` |
| P07 | [Identity, permissions, and agent access](plans/P07_identity_permissions_agents.md) | P01, P04 | complete | `build/treatcode-plan-evidence/P07/result.json` |
| P08 | [Remote development workspaces](plans/P08_remote_workspaces.md) | P03, P07 | complete | `build/treatcode-plan-evidence/P08/result.json` |
| P09 | [Secure execution and immutable evidence](plans/P09_secure_execution_evidence.md) | P07, P08 | complete | `build/treatcode-plan-evidence/P09/result.json` |
| P10 | [Optimization and benchmark lab](plans/P10_optimization_benchmarks.md) | P06, P09 | complete | `build/treatcode-plan-evidence/P10/result.json` |
| P11 | [Uploads, contributions, and GitHub](plans/P11_uploads_contributions_github.md) | P07, P08, P09 | complete | `build/treatcode-plan-evidence/P11/result.json` |
| P12 | [Operations and mobile collaboration](plans/P12_operations_mobile.md) | P08, P09, P11 | complete | `build/treatcode-plan-evidence/P12/result.json` |
| P13 | [Launch closure](plans/P13_launch_closure.md) | P04–P12 | complete | `build/treatcode-plan-evidence/P13/result.json` |
| P15 | [P04 completeness amendment — full Stack Explorer](plans/P15_P04_completeness_amendment.md) | P02, P03, P04 | complete | `build/treatcode-plan-evidence/P15/result.json` |
| P16 | [P05 completeness amendment — full learning curriculum](plans/P16_P05_curriculum_completeness_amendment.md) | P03, P04, P05, P15 | in_progress | `build/treatcode-plan-evidence/P16/result.json` |

## Completeness amendments

P04 and P05 remain complete historical v1 baselines under the change-control
rule. Their records establish that the original plumbing and representative
content slices worked; they do not satisfy the broader user-facing goals now
requested. P15 and P16 are the execution authority for those complete goals.
An agent must finish P15 and P16, including their coverage and human-review
evidence, before describing the Stack Explorer or Learn experience as complete.

After both amendments are complete, P13 launch closure must be re-run against
the amended evidence before issuing a new launch claim.

Paste-ready handoff prompts are available for the original agents in [the P04
prompt](plans/P04_AGENT_COMPLETION_PROMPT.md) and [the P05
prompt](plans/P05_AGENT_COMPLETION_PROMPT.md).

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
