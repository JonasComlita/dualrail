# P13 — Launch Closure

## Metadata

- **Plan ID:** P13
- **Version:** 1
- **Status:** `in_progress`
- **Depends on:** P04, P05, P06, P07, P08, P09, P10, P11, P12
- **Scope owner:** Release authority

## Objective

Prove that TreatCode can launch as a truthful, fast, secure, remotely operable,
human-and-agent platform without relying on demo data or bypassing evidence.

## Dependency Evidence Required

- Completion evidence for P04–P12 at compatible commits.

## Inputs and Authority

- All completed plan evidence
- Root acceptance criteria and production tests
- Security, privacy, licensing, incident, and release policies

## Deliverables

1. Launch readiness matrix mapping every public claim and user journey to
   authoritative evidence.
2. Production deployment, rollback, incident, and support procedures.
3. Security, privacy, licensing, accessibility, and contributor policies.
4. Final performance, security, recovery, and full-system reports.
5. Named launch approval and release record.

The controlled launch artifacts are kept under
`docs/11_TreatCode_Platform/launch/`: the readiness matrix, operational
procedure, policy bundle, release record, and final performance, security,
recovery, and full-system reports. The machine-readable launch report and
verification bundle are written under `build/treatcode-plan-evidence/P13/`.

## Non-Goals

- Completing every future Trit capability.
- Treating roadmap or research features as production.
- Waiving failed gates to meet a date.

## Acceptance Criteria

- [ ] P04–P12 verify complete at the launch candidate commit or compatible
      recorded dependency commits.
- [ ] Public statistics and status labels come from authoritative data; seeded
      demo values are absent or explicitly labeled demo.
- [ ] Published challenges have validated correctness contracts.
- [ ] Public code execution passes P09 isolation and abuse gates.
- [ ] Remote contribution cannot bypass P11 review boundaries.
- [ ] Production, smoke, public E2E, accessibility, performance, backup/restore,
      rollback, and security tests pass.
- [ ] Mobile and desktop critical user journeys pass.
- [ ] Every public roadmap feature is labeled by decision, maturity, evidence,
      and compatibility status.
- [ ] Release authority records an explicit go/no-go decision.

## Verification

```powershell
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
tools/trit-test.ps1 production
python tools/trit_tool.py knowledge status
npm --prefix treatcode run test:launch
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P13
```

## Required Evidence

- Launch readiness matrix.
- Full verification bundle and hashes.
- Release-authority decision.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Product owner; architecture owner; security owner; operations owner; release authority
- **Date:**
