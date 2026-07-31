# TreatCode Platform Domain Model v1

## Common entity envelope

Every platform entity has the following fields:

| Field | Contract |
|---|---|
| `schema_version` | `treatcode.platform.entity.v1` for this release. |
| `id` | Stable namespaced ID: `tc:<entity-type>:<slug>`. |
| `entity_type` | One of the 15 v1 entity types. The namespace in `id` must match. |
| `name`, `description` | Human-readable labels; identity is never derived from them. |
| `statuses` | Four independent lifecycle dimensions: decision, maturity, evidence, compatibility. |
| `status_history` | At least one timestamped state with the exact commit that recorded it. The final event must equal `statuses`. |
| `relations` | Zero or more typed edges with an existing namespaced `target_id`. |
| `source_refs` | One or more repository/commit/path-or-hash references for authoritative source. |
| `evidence_refs` | One or more repository/commit/path-or-hash references for proof or review evidence. |
| `tags`, `metadata` | Optional navigation and forward-compatible annotations. |

The complete machine contract is
[platform_domain.v1.schema.json](schemas/platform_domain.v1.schema.json).
The schema intentionally allows entity-specific extension fields while
requiring the common envelope and the required fields for each entity branch.

## Entity inventory

| Type | Stable ID example | Required domain-specific fields | Meaning |
|---|---|---|---|
| Project | `tc:project:trit` | `repository` | A repository-backed product or codebase. |
| Layer | `tc:layer:compiler` | `ordinal`, `layer_kind` | An ordered stack layer; new layers are data records. |
| Component | `tc:component:host-runtime` | `layer_id`, `component_kind` | A concrete unit within a layer. |
| Capability | `tc:capability:cross-layer-syscall-abi` | `layer_ids`, `capability_kind` | A user or system ability, including cross-layer abilities. |
| Contract | `tc:contract:syscall-abi-v1` | `contract_kind`, `contract_version`, `surface` | A versioned boundary or invariant. |
| Implementation | `tc:implementation:runtime-syscalls` | `component_id`, `implementation_kind` | A concrete implementation of a contract or capability. |
| Decision | `tc:decision:legacy-abi` | `question`, `resolution`, `decided_at` | A resolved architecture or product choice. |
| Proposal | `tc:proposal:ternary-native-text` | `problem`, `proposed_by` | An unresolved candidate decision. |
| Test | `tc:test:syscall-abi` | `command`, `asserts` | An executable or reviewable verification definition. |
| Benchmark | `tc:benchmark:os-platform` | `workload`, `metric`, `budget` | A repeatable performance or capacity definition. |
| Run | `tc:run:os-platform-2026-07-30` | `run_kind`, `started_at`, `result` | One execution of a test, benchmark, build, or import. |
| Artifact | `tc:artifact:p01-schema-evidence` | `artifact_kind`, `artifact_hash` | An immutable output identified by content hash. |
| Release | `tc:release:platform-contracts-v1` | `release_version`, `released_at` | A named distribution boundary containing artifacts. |
| Workspace | `tc:workspace:p01-agent-workspace` | `repository`, `base_commit`, `mode` | A scoped working context for a human or agent. |
| Task | `tc:task:p01-domain-contracts` | `scope`, `acceptance`, `priority` | A bounded unit of work with explicit acceptance. |

## Typed relations

Relations are directed from the containing entity to `target_id`.

| Relation | Allowed target types | Interpretation |
|---|---|---|
| `depends_on` | Any entity | The source requires the target before it can be understood or executed. |
| `implements` | Contract, capability, component | The source provides the target's behavior or boundary. |
| `produces` | Artifact, run, release | The source creates or records the target. |
| `consumes` | Artifact, capability, contract, component | The source uses the target as an input. |
| `verified_by` | Test, run | The source has a verification definition or result. |
| `benchmarked_by` | Benchmark, run | The source is measured by the target. |
| `supersedes` | Decision, proposal, contract | The source replaces the target's authority. |
| `compatible_with` | Contract, capability, implementation, layer, component | The source is known to interoperate with the target. |
| `affects` | Any entity | The source changes the scope, behavior, or decision context of the target. |
| `included_in_release` | Release | The source is shipped or recorded in the target release. |

Missing targets and target-type violations are semantic errors even when the
individual JSON objects satisfy their structural schema.

## Status model and transitions

The four dimensions are independent. A status event records all four values;
the verifier compares each consecutive event against its dimension's allowed
transition table.

- Decision: `proposed -> accepted -> superseded`, with `rejected` or
  `withdrawn` terminal. An accepted decision may also be rejected before it is
  superseded.
- Maturity: `planned -> specified -> implemented -> integrated -> tested ->
  benchmarked -> released`; a record may move to `deprecated` from an active
  maturity state.
- Evidence: `none -> claimed -> partial -> verified`; disputed evidence may be
  returned to claimed or partial after review.
- Compatibility: `unknown -> experimental -> compatible`, with explicit
  incompatible and deprecated outcomes.

Repeating a status is valid. Regressions such as `accepted -> proposed` or
`released -> planned` are rejected. The negative fixture
[p01_invalid_status_transition.v1.json](fixtures/p01_invalid_status_transition.v1.json)
demonstrates this rule.

## Provenance

Every reference has:

```json
{
  "repository": "https://github.com/JonasComlita/dualrail",
  "commit": "d168bc845babad7d6031bed98a16e3a471d200c5",
  "path": "SYSCALL_MANIFEST.json"
}
```

The path may be replaced by an immutable `artifact_hash` such as
`sha256:<64 hexadecimal digits>`. A repository without a commit, or a commit
without a path or immutable hash, is not evidence.

## Fixtures and extension check

[p01_valid_platform_graph.v1.json](fixtures/p01_valid_platform_graph.v1.json)
contains Trit records, a synthetic cross-layer capability, a superseded
decision, a benchmark and run, and a scoped workspace/task pair. The fixture
also exercises every typed relation. The three negative fixtures cover missing
relation targets, invalid lifecycle transitions, and incomplete provenance.

Run them with:

```powershell
python tools/trit_tool.py website schemas validate
python tools/trit_tool.py website schemas test-fixtures
```
