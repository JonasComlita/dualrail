# TreatCode Platform Product Charter

## Purpose

TreatCode is the official human-and-agent workspace for learning, querying,
developing, testing, benchmarking, reviewing, and releasing the Trit stack.
The platform must make repository truth, architectural intent, executable
evidence, and scoped work visible without turning any of those things into
unversioned website prose.

This charter is the product boundary for the platform-contract work in P01.
It does not select an API framework, database, hosting vendor, or UI design.

## Product promises

1. A reader can identify what a Trit project, stack layer, capability, or
   contract means and which source commit supports it.
2. An engineer can trace a capability across components, implementations,
   tests, benchmarks, artifacts, and releases.
3. An agent receives a bounded task, an explicit workspace, authoritative
   inputs, acceptance checks, and an evidence destination.
4. A decision can be proposed, accepted, superseded, rejected, or withdrawn
   without confusing that decision state with implementation maturity, proof
   strength, or compatibility.
5. A future layer or capability can be added as data that validates against the
   same versioned contracts; no page or database migration is required for the
   domain model itself.

## Contract principles

### Repository-backed truth

Every source or evidence reference carries a repository, an exact commit, and
either a repository path or an immutable artifact hash. A page may summarize a
record, but it cannot silently replace the referenced source, manifest, test
result, or release artifact.

### Stable identity

Domain records use IDs in the form `tc:<entity-type>:<slug>`, for example
`tc:capability:cross-layer-syscall-abi`. IDs are stable across display-name
changes. The schema version is carried in every entity so a reader can tell
which contract interpreted the record.

### Separate lifecycle dimensions

Every entity exposes four independent status dimensions:

- `decision`: proposed, accepted, superseded, rejected, or withdrawn;
- `maturity`: planned through specified, implemented, integrated, tested,
  benchmarked, released, or deprecated;
- `evidence`: none, claimed, partial, verified, or disputed; and
- `compatibility`: unknown, incompatible, experimental, compatible, or
  deprecated.

No dimension is inferred from another. For example, an accepted proposal is
not automatically implemented, and a compatible implementation is not
automatically verified.

### Explicit relationships

Relations are typed rather than encoded in prose. P01 defines
`depends_on`, `implements`, `produces`, `consumes`, `verified_by`,
`benchmarked_by`, `supersedes`, `compatible_with`, `affects`, and
`included_in_release`. A relation target must exist in the graph and must be a
valid target type for that relation.

### Machine-verifiable completion

P00's plan verifier is the completion gate. P01 adds its schema catalog and
positive/negative fixtures to that gate. Human approval remains an explicit
recorded gate; an automated fixture result never impersonates product or
architecture approval.

## Initial contract inventory

The v1 domain covers projects, stack layers, components, capabilities,
contracts, implementations, decisions, proposals, tests, benchmarks, runs,
artifacts, releases, workspaces, and tasks. The normative field and relation
definitions are in [DOMAIN_MODEL.md](DOMAIN_MODEL.md) and
[schemas/platform_domain.v1.schema.json](schemas/platform_domain.v1.schema.json).

## Extension rule

To add a new entity type, a later plan must add a namespaced ID branch, typed
relations, source/evidence rules, a lifecycle fixture, and verifier coverage.
To add a new layer or capability, data may use the existing `layer` or
`capability` schema without a code or UI change. The
[P01 valid graph fixture](fixtures/p01_valid_platform_graph.v1.json) includes
that synthetic-extension shape alongside Trit records.

## Ownership and approval

The product owner approves the user-facing promises and scope. The architecture
owner approves the entity boundaries, status semantics, and relation model.
Those approvals must be recorded with reviewer, decision, date, and commit in
the plan manifest before P01 is marked complete. This document is the
machine-checked charter draft until those named approvals are recorded.
