#!/usr/bin/env python3
"""Validation for the versioned TreatCode challenge source of truth."""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path
from typing import Any

try:
    from treatcode_platform import validate_json_instance
except ModuleNotFoundError:  # package import used by host tooling tests
    from tools.treatcode_platform import validate_json_instance


REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "CHALLENGE_MANIFEST.json"
SCHEMA_PATH = REPO_ROOT / "CHALLENGE_MANIFEST_SCHEMA.json"
DIMENSIONS = ("domain", "technique", "data_model", "stack_layer", "target", "optimization_objective")
LIFECYCLES = {"published", "draft", "retired"}


def _issue(code: str, message: str, path: str = "$", **details: Any) -> dict[str, Any]:
    issue = {"code": code, "message": message, "path": path}
    issue.update(details)
    return issue


def _load(path: Path) -> tuple[Any | None, str | None]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except FileNotFoundError:
        return None, "file is missing"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"


def _relative(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def validate_challenge_manifest(
    manifest_path: Path = MANIFEST_PATH,
    schema_path: Path = SCHEMA_PATH,
) -> dict[str, Any]:
    manifest, manifest_error = _load(manifest_path)
    schema, schema_error = _load(schema_path)
    errors: list[dict[str, Any]] = []
    if manifest_error:
        errors.append(_issue("manifest.load", f"{_relative(manifest_path)}: {manifest_error}"))
    if schema_error:
        errors.append(_issue("schema.load", f"{_relative(schema_path)}: {schema_error}"))
    if errors:
        return {"schema": "trit.challenge_validation.v1", "ok": False, "manifest": _relative(manifest_path), "errors": errors}
    assert isinstance(manifest, dict)
    assert isinstance(schema, dict)

    errors.extend(
        {
            **error,
            "code": f"schema.{error.get('code', 'invalid')}",
        }
        for error in validate_json_instance(
            manifest,
            schema,
            root=schema,
            source_path=schema_path,
        )
    )

    challenges = manifest.get("challenges", [])
    dimensions = manifest.get("facet_dimensions", {})
    ids: list[str] = []
    lifecycle_counts: Counter[str] = Counter()
    published_ids: list[str] = []
    draft_ids: list[str] = []
    retired_ids: list[str] = []
    for index, challenge in enumerate(challenges if isinstance(challenges, list) else []):
        path = f"$.challenges[{index}]"
        if not isinstance(challenge, dict):
            errors.append(_issue("challenge.type", "challenge must be an object", path))
            continue
        challenge_id = str(challenge.get("id", ""))
        ids.append(challenge_id)
        lifecycle = challenge.get("lifecycle")
        lifecycle_counts[str(lifecycle)] += 1
        if lifecycle == "published":
            published_ids.append(challenge_id)
        elif lifecycle == "draft":
            draft_ids.append(challenge_id)
        elif lifecycle == "retired":
            retired_ids.append(challenge_id)

        facets = challenge.get("facets", {})
        for dimension in DIMENSIONS:
            values = facets.get(dimension, []) if isinstance(facets, dict) else []
            known = set(dimensions.get(dimension, [])) if isinstance(dimensions, dict) else set()
            for value in values if isinstance(values, list) else []:
                if value not in known:
                    errors.append(
                        _issue(
                            "facet.unknown",
                            f"facet value {value!r} is not declared in {dimension}",
                            f"{path}.facets.{dimension}",
                            challenge_id=challenge_id,
                        )
                    )

        execution = challenge.get("execution", {})
        correctness = execution.get("correctness") if isinstance(execution, dict) else None
        tests = correctness.get("test_cases") if isinstance(correctness, dict) else None
        if lifecycle == "published":
            if not isinstance(execution, dict) or execution.get("mode") != "verified":
                errors.append(_issue("published.mode", "published challenge must use verified execution", f"{path}.execution"))
            if not isinstance(tests, list) or not tests:
                errors.append(_issue("published.correctness", "published challenge must declare deterministic test cases", f"{path}.execution.correctness"))
            if not isinstance(correctness, dict) or correctness.get("kind") != "deterministic-vm-contract":
                errors.append(_issue("published.contract", "published challenge must use the deterministic VM contract", f"{path}.execution.correctness"))
            if isinstance(tests, list):
                seen_cases: set[str] = set()
                for case_index, case in enumerate(tests):
                    if not isinstance(case, dict):
                        continue
                    case_key = json.dumps(case.get("arg"), sort_keys=True, separators=(",", ":"))
                    if case_key in seen_cases:
                        errors.append(_issue("published.duplicate_case", "published test cases must be distinct", f"{path}.execution.correctness.test_cases[{case_index}]"))
                    seen_cases.add(case_key)
                    has_expected = "expected_output" in case or "expected_r13" in case
                    if not has_expected:
                        errors.append(_issue("published.expected", "every published test case needs an expected output or R13 value", f"{path}.execution.correctness.test_cases[{case_index}]"))
            if isinstance(execution, dict) and not isinstance(execution.get("limits"), dict):
                errors.append(_issue("published.limits", "published challenge must declare execution limits", f"{path}.execution.limits"))
        elif lifecycle in {"draft", "retired"}:
            if isinstance(execution, dict) and execution.get("mode") == "verified":
                errors.append(_issue("unpublished.mode", "draft and retired challenges cannot use verified execution", f"{path}.execution"))
            if correctness is not None:
                errors.append(_issue("unpublished.correctness", "draft and retired challenges cannot expose a correctness contract", f"{path}.execution.correctness"))

    for challenge_id, count in Counter(ids).items():
        if not challenge_id:
            continue
        if count > 1:
            errors.append(_issue("challenge.duplicate_id", f"challenge id {challenge_id} appears {count} times", "$.challenges", challenge_id=challenge_id))

    if not published_ids:
        errors.append(_issue("coverage.no_published", "manifest must contain at least one published challenge"))
    if not draft_ids:
        errors.append(_issue("coverage.no_draft", "manifest must contain at least one draft challenge"))
    if not retired_ids:
        errors.append(_issue("coverage.no_retired", "manifest must contain at least one retired challenge"))

    tritwise_pilot = any(
        isinstance(challenge, dict)
        and "tritwise" in challenge.get("facets", {}).get("technique", [])
        and "word-parallel-trits" in challenge.get("facets", {}).get("data_model", [])
        and challenge.get("lifecycle") == "published"
        for challenge in challenges
    )
    vector_pilot = any(
        isinstance(challenge, dict)
        and "vector-dot-product" in challenge.get("facets", {}).get("technique", [])
        and "vectors" in challenge.get("facets", {}).get("data_model", [])
        and challenge.get("lifecycle") == "published"
        for challenge in challenges
    )
    if not tritwise_pilot:
        errors.append(_issue("coverage.tritwise_pilot", "a published word-parallel tritwise pilot is required"))
    if not vector_pilot:
        errors.append(_issue("coverage.vector_pilot", "a published vector dot-product or matrix pilot is required"))

    return {
        "schema": "trit.challenge_validation.v1",
        "ok": not errors,
        "manifest": _relative(manifest_path),
        "schema_path": _relative(schema_path),
        "manifest_schema": manifest.get("schema"),
        "version": manifest.get("version"),
        "challenges": len(challenges) if isinstance(challenges, list) else 0,
        "lifecycle_counts": dict(sorted(lifecycle_counts.items())),
        "published_ids": published_ids,
        "draft_ids": draft_ids,
        "retired_ids": retired_ids,
        "facet_dimensions": {dimension: len(dimensions.get(dimension, [])) for dimension in DIMENSIONS} if isinstance(dimensions, dict) else {},
        "pilots": {"tritwise_word_parallel": tritwise_pilot, "vector_dot_product": vector_pilot},
        "errors": errors,
    }


__all__ = ["MANIFEST_PATH", "SCHEMA_PATH", "validate_challenge_manifest"]
