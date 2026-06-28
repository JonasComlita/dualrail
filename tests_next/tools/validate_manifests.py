#!/usr/bin/env python3
"""Validate tests_next manifest files.

This intentionally uses only the Python standard library so any agent can run
it without dependency setup.
"""

from __future__ import annotations

import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFESTS = ROOT / "manifests"


def load_json(path: pathlib.Path):
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except Exception as exc:  # pragma: no cover - command-line reporting
        raise SystemExit(f"failed to load {path}: {exc}") from exc


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def main() -> int:
    failures: list[str] = []

    status = load_json(MANIFESTS / "status.json")
    gates = load_json(MANIFESTS / "acceptance_gates.json")
    coverage = load_json(MANIFESTS / "coverage_matrix.json")
    template = load_json(MANIFESTS / "test_case.template.json")
    test_cases_path = MANIFESTS / "test_cases.json"
    test_cases = load_json(test_cases_path) if test_cases_path.exists() else None

    require(status.get("schema") == "trit.tests_next.status.v1",
            "status.json has unexpected schema", failures)
    require(gates.get("schema") == "trit.tests_next.acceptance_gates.v1",
            "acceptance_gates.json has unexpected schema", failures)
    require(coverage.get("schema") == "trit.tests_next.coverage_matrix.v1",
            "coverage_matrix.json has unexpected schema", failures)

    gate_names = {gate.get("name") for gate in gates.get("gates", [])}
    require({"ci_fast", "ci_full", "ci_release"}.issubset(gate_names),
            "acceptance gates must include ci_fast, ci_full, and ci_release", failures)

    valid_statuses = set(coverage.get("status_values", []))
    claim_ids: set[str] = set()
    for claim in coverage.get("claims", []):
        claim_id = claim.get("id")
        require(isinstance(claim_id, str) and "." in claim_id,
                f"claim has invalid id: {claim_id!r}", failures)
        require(claim_id not in claim_ids, f"duplicate claim id: {claim_id}", failures)
        claim_ids.add(claim_id)
        require(claim.get("status") in valid_statuses,
                f"{claim_id} has invalid status {claim.get('status')!r}", failures)
        require(isinstance(claim.get("required_new_coverage"), list) and
                len(claim.get("required_new_coverage", [])) > 0,
                f"{claim_id} must list required_new_coverage", failures)

    for field in ["id", "layer", "feature", "kind", "source", "proves", "gate"]:
        require(field in template, f"test case template missing {field}", failures)

    test_count = 0
    if test_cases is not None:
        require(test_cases.get("schema") == "trit.tests_next.test_cases.v1",
                "test_cases.json has unexpected schema", failures)
        valid_kinds = {"focused", "group", "integration", "full_system", "fuzz", "perf"}
        test_ids: set[str] = set()
        for case in test_cases.get("tests", []):
            test_count += 1
            test_id = case.get("id")
            require(isinstance(test_id, str) and "." in test_id,
                    f"test case has invalid id: {test_id!r}", failures)
            require(test_id not in test_ids, f"duplicate test id: {test_id}", failures)
            test_ids.add(test_id)
            require(case.get("kind") in valid_kinds,
                    f"{test_id} has invalid kind {case.get('kind')!r}", failures)
            require(case.get("gate") in gate_names,
                    f"{test_id} references unknown gate {case.get('gate')!r}", failures)
            require(isinstance(case.get("proves"), list) and len(case.get("proves", [])) > 0,
                    f"{test_id} must list proved invariants", failures)
            case_claims = case.get("claims")
            require(isinstance(case_claims, list) and len(case_claims) > 0,
                    f"{test_id} must map to at least one coverage claim", failures)
            if isinstance(case_claims, list):
                for claim in case_claims:
                    require(claim in claim_ids,
                            f"{test_id} references unknown claim {claim!r}", failures)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print("tests_next manifests OK")
    print(f"claims: {len(claim_ids)}")
    print(f"gates: {', '.join(sorted(gate_names))}")
    if test_cases is not None:
        print(f"test cases: {test_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
