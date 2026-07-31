#!/usr/bin/env python3
"""Validation and evidence tooling for the TreatCode platform plans.

The platform contracts intentionally use ordinary JSON Schema documents, but
the repository does not require a third-party Python dependency just to run
its pre-build tooling.  This module therefore contains a small, strict
validator for the JSON Schema vocabulary used by the checked-in contracts and
the semantic checks that JSON Schema cannot express (relation targets and
status transitions).
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import platform
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
PLATFORM_ROOT = REPO_ROOT / "docs" / "11_TreatCode_Platform"
SCHEMAS_DIR = PLATFORM_ROOT / "schemas"
FIXTURES_DIR = PLATFORM_ROOT / "fixtures"
PLAN_MANIFEST_PATH = PLATFORM_ROOT / "TREATCODE_PLAN_MANIFEST.json"
PLAN_MANIFEST_SCHEMA_PATH = SCHEMAS_DIR / "treatcode_plan_manifest.v1.schema.json"
PLAN_INDEX_PATH = PLATFORM_ROOT / "PLAN_INDEX.md"
EVIDENCE_ROOT = REPO_ROOT / "build" / "treatcode-plan-evidence"

ENTITY_TYPES = (
    "project",
    "layer",
    "component",
    "capability",
    "contract",
    "implementation",
    "decision",
    "proposal",
    "test",
    "benchmark",
    "run",
    "artifact",
    "release",
    "workspace",
    "task",
)
RELATION_TYPES = (
    "depends_on",
    "implements",
    "produces",
    "consumes",
    "verified_by",
    "benchmarked_by",
    "supersedes",
    "compatible_with",
    "affects",
    "included_in_release",
)
STATUS_KEYS = ("decision", "maturity", "evidence", "compatibility")
STATUS_VALUES = {
    "decision": {"proposed", "accepted", "superseded", "rejected", "withdrawn"},
    "maturity": {
        "planned",
        "specified",
        "implemented",
        "integrated",
        "tested",
        "benchmarked",
        "released",
        "deprecated",
    },
    "evidence": {"none", "claimed", "partial", "verified", "disputed"},
    "compatibility": {"unknown", "incompatible", "experimental", "compatible", "deprecated"},
}

# A transition may remain at the same value.  The relation graph is deliberately
# more permissive than this table; these are lifecycle transitions, not an
# attempt to encode every domain-specific policy in the schema.
STATUS_TRANSITIONS = {
    "decision": {
        "proposed": {"proposed", "accepted", "rejected", "withdrawn"},
        "accepted": {"accepted", "superseded", "rejected"},
        "superseded": {"superseded"},
        "rejected": {"rejected"},
        "withdrawn": {"withdrawn"},
    },
    "maturity": {
        "planned": {"planned", "specified", "implemented", "deprecated"},
        "specified": {"specified", "implemented", "deprecated"},
        "implemented": {"implemented", "integrated", "deprecated"},
        "integrated": {"integrated", "tested", "deprecated"},
        "tested": {"tested", "benchmarked", "released", "deprecated"},
        "benchmarked": {"benchmarked", "released", "deprecated"},
        "released": {"released", "deprecated"},
        "deprecated": {"deprecated"},
    },
    "evidence": {
        "none": {"none", "claimed"},
        "claimed": {"claimed", "partial", "verified", "disputed"},
        "partial": {"partial", "verified", "disputed"},
        "verified": {"verified", "disputed"},
        "disputed": {"disputed", "claimed", "partial"},
    },
    "compatibility": {
        "unknown": {"unknown", "incompatible", "experimental", "compatible"},
        "incompatible": {"incompatible", "experimental", "compatible", "deprecated"},
        "experimental": {"experimental", "incompatible", "compatible", "deprecated"},
        "compatible": {"compatible", "deprecated"},
        "deprecated": {"deprecated"},
    },
}

RELATION_TARGET_TYPES = {
    "depends_on": set(ENTITY_TYPES),
    "implements": {"contract", "capability", "component"},
    "produces": {"artifact", "run", "release"},
    "consumes": {"artifact", "capability", "contract", "component"},
    "verified_by": {"test", "run"},
    "benchmarked_by": {"benchmark", "run"},
    "supersedes": {"decision", "proposal", "contract"},
    "compatible_with": {"contract", "capability", "implementation", "layer", "component"},
    "affects": set(ENTITY_TYPES),
    "included_in_release": {"release"},
}

ID_RE = re.compile(
    r"^tc:(?P<type>project|layer|component|capability|contract|implementation|decision|proposal|test|benchmark|run|artifact|release|workspace|task):(?P<slug>[a-z0-9][a-z0-9-]*(?:\.[a-z0-9][a-z0-9-]*)*)$"
)
COMMIT_RE = re.compile(r"^[0-9a-fA-F]{7,64}$")
ARTIFACT_HASH_RE = re.compile(r"^(?:sha256|sha512):[0-9a-fA-F]{64,128}$")


def _load_json(path: Path) -> tuple[Any | None, str | None]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except FileNotFoundError:
        return None, "file is missing"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"


def _issue(code: str, message: str, path: str = "$", **extra: Any) -> dict[str, Any]:
    result = {"code": code, "path": path, "message": message}
    result.update(extra)
    return result


def _path_join(path: str, key: Any) -> str:
    if isinstance(key, int):
        return f"{path}[{key}]"
    if path == "$":
        return f"$.{key}"
    return f"{path}.{key}"


def _type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    return True


def _resolve_pointer(document: Any, pointer: str) -> Any:
    if pointer in ("", "/"):
        return document
    current = document
    for token in pointer.lstrip("/").split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(current, list):
            current = current[int(token)]
        else:
            current = current[token]
    return current


def _resolve_ref(
    ref: str,
    root: Any,
    source_path: Path,
    cache: dict[Path, Any],
) -> tuple[Any, Any, Path]:
    if ref.startswith("#"):
        return _resolve_pointer(root, ref[1:]), root, source_path
    if "#" in ref:
        file_part, fragment = ref.split("#", 1)
    else:
        file_part, fragment = ref, ""
    target_path = (source_path.parent / file_part).resolve()
    if target_path not in cache:
        loaded, error = _load_json(target_path)
        if error:
            raise ValueError(f"{ref}: {error}")
        cache[target_path] = loaded
    target_root = cache[target_path]
    return _resolve_pointer(target_root, fragment), target_root, target_path


def validate_json_instance(
    value: Any,
    schema: Any,
    *,
    root: Any | None = None,
    source_path: Path | None = None,
    path: str = "$",
    cache: dict[Path, Any] | None = None,
) -> list[dict[str, Any]]:
    """Validate the JSON Schema subset used by the checked-in contracts."""

    if root is None:
        root = schema
    if source_path is None:
        source_path = PLAN_MANIFEST_SCHEMA_PATH
    if cache is None:
        cache = {source_path.resolve(): root}

    if not isinstance(schema, dict):
        return [_issue("schema.invalid", "schema node must be an object", path)]

    if "$ref" in schema:
        try:
            target, target_root, target_path = _resolve_ref(str(schema["$ref"]), root, source_path, cache)
        except (KeyError, IndexError, ValueError) as exc:
            return [_issue("schema.ref", str(exc), path)]
        return validate_json_instance(
            value,
            target,
            root=target_root,
            source_path=target_path,
            path=path,
            cache=cache,
        )

    errors: list[dict[str, Any]] = []
    if "const" in schema and value != schema["const"]:
        errors.append(_issue("value.const", f"must equal {schema['const']!r}", path))
    if "enum" in schema and value not in schema["enum"]:
        errors.append(_issue("value.enum", f"must be one of {schema['enum']!r}", path))

    expected_type = schema.get("type")
    if expected_type is not None:
        types = expected_type if isinstance(expected_type, list) else [expected_type]
        if not any(_type_matches(value, str(item)) for item in types):
            return errors + [_issue("type", f"must be of type {expected_type!r}", path)]

    if "allOf" in schema:
        for index, child in enumerate(schema["allOf"]):
            errors.extend(
                validate_json_instance(
                    value,
                    child,
                    root=root,
                    source_path=source_path,
                    path=path,
                    cache=cache,
                )
            )

    if "anyOf" in schema:
        valid = 0
        child_errors: list[list[dict[str, Any]]] = []
        for child in schema["anyOf"]:
            child_result = validate_json_instance(
                value,
                child,
                root=root,
                source_path=source_path,
                path=path,
                cache=cache,
            )
            if not child_result:
                valid += 1
            child_errors.append(child_result)
        if valid == 0:
            errors.append(_issue("anyOf", "must satisfy at least one schema", path, alternatives=child_errors))

    if "oneOf" in schema:
        valid = 0
        child_errors = []
        for child in schema["oneOf"]:
            child_result = validate_json_instance(
                value,
                child,
                root=root,
                source_path=source_path,
                path=path,
                cache=cache,
            )
            if not child_result:
                valid += 1
            child_errors.append(child_result)
        if valid != 1:
            errors.append(
                _issue(
                    "oneOf",
                    f"must satisfy exactly one schema; matched {valid}",
                    path,
                    alternatives=child_errors,
                )
            )

    if "not" in schema:
        if not validate_json_instance(value, schema["not"], root=root, source_path=source_path, path=path, cache=cache):
            errors.append(_issue("not", "must not satisfy the excluded schema", path))

    if isinstance(value, str):
        if "minLength" in schema and len(value) < int(schema["minLength"]):
            errors.append(_issue("string.minLength", "is shorter than the minimum length", path))
        if "maxLength" in schema and len(value) > int(schema["maxLength"]):
            errors.append(_issue("string.maxLength", "is longer than the maximum length", path))
        if "pattern" in schema:
            try:
                matches = re.search(str(schema["pattern"]), value)
            except re.error as exc:
                errors.append(_issue("schema.pattern", str(exc), path))
                matches = True
            if not matches:
                errors.append(_issue("string.pattern", "does not match the required pattern", path))
        if schema.get("format") == "date-time":
            try:
                _dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
            except ValueError:
                errors.append(_issue("string.date-time", "must be an ISO-8601 date-time", path))

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(_issue("number.minimum", "is below the minimum", path))
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(_issue("number.maximum", "is above the maximum", path))

    if isinstance(value, list):
        if "minItems" in schema and len(value) < int(schema["minItems"]):
            errors.append(_issue("array.minItems", "has fewer items than required", path))
        if "maxItems" in schema and len(value) > int(schema["maxItems"]):
            errors.append(_issue("array.maxItems", "has more items than allowed", path))
        if schema.get("uniqueItems"):
            encoded = [json.dumps(item, sort_keys=True) for item in value]
            if len(encoded) != len(set(encoded)):
                errors.append(_issue("array.uniqueItems", "items must be unique", path))
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                errors.extend(
                    validate_json_instance(
                        item,
                        item_schema,
                        root=root,
                        source_path=source_path,
                        path=_path_join(path, index),
                        cache=cache,
                    )
                )

    if isinstance(value, dict):
        properties = schema.get("properties", {})
        if not isinstance(properties, dict):
            errors.append(_issue("schema.properties", "properties must be an object", path))
            properties = {}
        for required in schema.get("required", []):
            if required not in value:
                errors.append(_issue("object.required", f"missing required property {required!r}", _path_join(path, required)))
        for key, child_schema in properties.items():
            if key in value:
                errors.extend(
                    validate_json_instance(
                        value[key],
                        child_schema,
                        root=root,
                        source_path=source_path,
                        path=_path_join(path, key),
                        cache=cache,
                    )
                )
        if schema.get("additionalProperties") is False:
            allowed = set(properties)
            for key in value:
                if key not in allowed:
                    errors.append(_issue("object.additionalProperties", f"unexpected property {key!r}", _path_join(path, key)))
        elif isinstance(schema.get("additionalProperties"), dict):
            allowed = set(properties)
            for key, child in value.items():
                if key not in allowed:
                    errors.extend(
                        validate_json_instance(
                            child,
                            schema["additionalProperties"],
                            root=root,
                            source_path=source_path,
                            path=_path_join(path, key),
                            cache=cache,
                        )
                    )
    return errors


def _iter_schema_files() -> Iterable[Path]:
    return sorted(SCHEMAS_DIR.glob("*.schema.json"))


def validate_schema_catalog() -> dict[str, Any]:
    errors: list[dict[str, Any]] = []
    files = list(_iter_schema_files())
    ids: set[str] = set()
    documents: dict[Path, Any] = {}
    for path in files:
        data, error = _load_json(path)
        if error:
            errors.append(_issue("schema.file", f"{path.name}: {error}"))
            continue
        documents[path.resolve()] = data
        if not isinstance(data, dict):
            errors.append(_issue("schema.document", f"{path.name}: top-level value must be an object"))
            continue
        for key in ("$schema", "$id", "title"):
            if not isinstance(data.get(key), str) or not data[key]:
                errors.append(_issue("schema.metadata", f"{path.name}: missing {key}"))
        schema_id = data.get("$id")
        if isinstance(schema_id, str):
            if schema_id in ids:
                errors.append(_issue("schema.duplicate_id", f"duplicate schema id {schema_id}"))
            ids.add(schema_id)
        if not ("type" in data or "$ref" in data or "oneOf" in data or "allOf" in data):
            errors.append(_issue("schema.root", f"{path.name}: no validation keyword at root"))

    domain_path = SCHEMAS_DIR / "platform_domain.v1.schema.json"
    domain = documents.get(domain_path.resolve())
    if not isinstance(domain, dict):
        errors.append(_issue("schema.domain_missing", "platform_domain.v1.schema.json is required"))
    else:
        definitions = domain.get("$defs")
        if not isinstance(definitions, dict):
            errors.append(_issue("schema.domain_defs", "domain schema must define $defs"))
        else:
            for key in ("entity_common", "relation", "source_ref", "evidence_ref", *ENTITY_TYPES):
                if key not in definitions:
                    errors.append(_issue("schema.domain_definition", f"domain schema is missing $defs/{key}"))
        root_schema = domain.get("oneOf")
        if not isinstance(root_schema, list) or len(root_schema) != len(ENTITY_TYPES):
            errors.append(_issue("schema.domain_entities", "domain schema must expose one branch for every entity type"))

    if not files:
        errors.append(_issue("schema.empty", "no versioned schema files found"))
    return {
        "ok": not errors,
        "schema_directory": str(SCHEMAS_DIR.relative_to(REPO_ROOT)).replace("\\", "/"),
        "files": [path.name for path in files],
        "schema_count": len(files),
        "errors": errors,
    }


def _validate_reference(reference: Any, *, path: str, kind: str) -> list[dict[str, Any]]:
    errors: list[dict[str, Any]] = []
    if not isinstance(reference, dict):
        return [_issue("reference.type", f"{kind} reference must be an object", path)]
    repository = reference.get("repository")
    commit = reference.get("commit")
    if not isinstance(repository, str) or not repository.strip():
        errors.append(_issue("reference.repository", "repository is required", _path_join(path, "repository")))
    if not isinstance(commit, str) or not COMMIT_RE.fullmatch(commit):
        errors.append(_issue("reference.commit", "commit must be a hexadecimal commit id", _path_join(path, "commit")))
    source_path = reference.get("path")
    artifact_hash = reference.get("artifact_hash")
    if not ((isinstance(source_path, str) and bool(source_path.strip())) or (isinstance(artifact_hash, str) and ARTIFACT_HASH_RE.fullmatch(artifact_hash))):
        errors.append(
            _issue(
                "reference.locator",
                "reference must include a non-empty path or an immutable sha256/sha512 artifact_hash",
                path,
            )
        )
    return errors


def _validate_statuses(statuses: Any, path: str) -> list[dict[str, Any]]:
    if not isinstance(statuses, dict):
        return [_issue("status.type", "statuses must be an object", path)]
    errors: list[dict[str, Any]] = []
    if set(statuses) != set(STATUS_KEYS):
        errors.append(_issue("status.keys", f"statuses must contain exactly {list(STATUS_KEYS)!r}", path))
    for key in STATUS_KEYS:
        if key not in statuses:
            continue
        if statuses[key] not in STATUS_VALUES[key]:
            errors.append(_issue("status.value", f"invalid {key} status {statuses[key]!r}", _path_join(path, key)))
    return errors


def _validate_status_history(entity: dict[str, Any], path: str) -> list[dict[str, Any]]:
    errors = _validate_statuses(entity.get("statuses"), _path_join(path, "statuses"))
    history = entity.get("status_history")
    if not isinstance(history, list) or not history:
        return errors + [_issue("status.history", "status_history must contain at least one event", _path_join(path, "status_history"))]
    previous: dict[str, str] | None = None
    for index, event in enumerate(history):
        event_path = _path_join(_path_join(path, "status_history"), index)
        if not isinstance(event, dict):
            errors.append(_issue("status.event", "status history event must be an object", event_path))
            continue
        statuses = event.get("statuses")
        errors.extend(_validate_statuses(statuses, _path_join(event_path, "statuses")))
        at = event.get("at")
        if not isinstance(at, str):
            errors.append(_issue("status.timestamp", "status event at is required", _path_join(event_path, "at")))
        else:
            try:
                _dt.datetime.fromisoformat(at.replace("Z", "+00:00"))
            except ValueError:
                errors.append(_issue("status.timestamp", "status event at must be ISO-8601", _path_join(event_path, "at")))
        commit = event.get("commit")
        if not isinstance(commit, str) or not COMMIT_RE.fullmatch(commit):
            errors.append(_issue("status.commit", "status event commit must be a hexadecimal commit id", _path_join(event_path, "commit")))
        if isinstance(statuses, dict):
            if previous is not None:
                for key in STATUS_KEYS:
                    before = previous.get(key)
                    after = statuses.get(key)
                    if before in STATUS_VALUES[key] and after in STATUS_VALUES[key] and after not in STATUS_TRANSITIONS[key][before]:
                        errors.append(
                            _issue(
                                "status.transition",
                                f"invalid {key} transition {before!r} -> {after!r}",
                                _path_join(event_path, "statuses"),
                            )
                        )
            previous = statuses
    if isinstance(previous, dict) and previous != entity.get("statuses"):
        errors.append(_issue("status.current_mismatch", "statuses must equal the last status_history event", _path_join(path, "statuses")))
    return errors


def _entity_type_from_id(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    match = ID_RE.fullmatch(value)
    return match.group("type") if match else None


def validate_platform_graph(graph: Any, *, source_path: Path | None = None) -> dict[str, Any]:
    errors: list[dict[str, Any]] = []
    if not isinstance(graph, dict):
        return {"ok": False, "entities": 0, "errors": [_issue("fixture.type", "fixture must be an object")]}
    if graph.get("schema_version") != "treatcode.platform.fixture.v1":
        errors.append(_issue("fixture.schema_version", "fixture schema_version must be treatcode.platform.fixture.v1", "$.schema_version"))
    fixture_id = graph.get("fixture_id")
    if not isinstance(fixture_id, str) or not re.fullmatch(r"tc:fixture:[a-z0-9][a-z0-9-]*", fixture_id):
        errors.append(_issue("fixture.id", "fixture_id must be namespaced as tc:fixture:<slug>", "$.fixture_id"))
    entities = graph.get("entities")
    if not isinstance(entities, list) or not entities:
        return {
            "ok": False,
            "fixture_id": fixture_id,
            "entities": 0,
            "errors": errors + [_issue("fixture.entities", "entities must be a non-empty array", "$.entities")],
        }

    domain_path = SCHEMAS_DIR / "platform_domain.v1.schema.json"
    domain, domain_error = _load_json(domain_path)
    if domain_error or not isinstance(domain, dict):
        errors.append(_issue("schema.domain", domain_error or "domain schema is not an object"))
        domain = {}

    by_id: dict[str, dict[str, Any]] = {}
    for index, entity in enumerate(entities):
        entity_path = _path_join("$.entities", index)
        if not isinstance(entity, dict):
            errors.append(_issue("entity.type", "entity must be an object", entity_path))
            continue
        if domain:
            schema_errors = validate_json_instance(
                entity,
                domain,
                root=domain,
                source_path=domain_path,
                path=entity_path,
                cache={domain_path.resolve(): domain},
            )
            errors.extend(schema_errors)
        entity_id = entity.get("id")
        entity_type = entity.get("entity_type")
        parsed_type = _entity_type_from_id(entity_id)
        if parsed_type is None:
            errors.append(_issue("entity.id", "id must be a stable namespaced platform id", _path_join(entity_path, "id")))
        elif parsed_type != entity_type:
            errors.append(_issue("entity.id_namespace", "id namespace must match entity_type", entity_path))
        if isinstance(entity_id, str):
            if entity_id in by_id:
                errors.append(_issue("entity.duplicate_id", f"duplicate entity id {entity_id!r}", _path_join(entity_path, "id")))
            else:
                by_id[entity_id] = entity
        errors.extend(_validate_status_history(entity, entity_path))
        for ref_index, reference in enumerate(entity.get("source_refs", [])):
            errors.extend(_validate_reference(reference, path=_path_join(_path_join(entity_path, "source_refs"), ref_index), kind="source"))
        for ref_index, reference in enumerate(entity.get("evidence_refs", [])):
            errors.extend(_validate_reference(reference, path=_path_join(_path_join(entity_path, "evidence_refs"), ref_index), kind="evidence"))

    for index, entity in enumerate(entities):
        if not isinstance(entity, dict):
            continue
        source_type = entity.get("entity_type")
        for relation_index, relation in enumerate(entity.get("relations", [])):
            relation_path = _path_join(_path_join(_path_join("$.entities", index), "relations"), relation_index)
            if not isinstance(relation, dict):
                continue
            relation_type = relation.get("type")
            target_id = relation.get("target_id")
            if target_id not in by_id:
                errors.append(_issue("relation.target_missing", f"relation target {target_id!r} does not exist in this fixture", _path_join(relation_path, "target_id")))
                continue
            target_type = by_id[target_id].get("entity_type")
            if relation_type in RELATION_TARGET_TYPES and target_type not in RELATION_TARGET_TYPES[relation_type]:
                errors.append(
                    _issue(
                        "relation.target_type",
                        f"{relation_type} from {source_type} cannot target {target_type}",
                        relation_path,
                    )
                )

    return {
        "ok": not errors,
        "fixture_id": fixture_id,
        "entities": len(entities),
        "entity_types": sorted({str(item.get("entity_type")) for item in entities if isinstance(item, dict)}),
        "relations": sum(len(item.get("relations", [])) for item in entities if isinstance(item, dict)),
        "errors": errors,
        "source": str(source_path.relative_to(REPO_ROOT)).replace("\\", "/") if source_path and source_path.is_relative_to(REPO_ROOT) else None,
    }


def _fixture_manifest() -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    path = FIXTURES_DIR / "manifest.v1.json"
    data, error = _load_json(path)
    if error:
        return None, [_issue("fixture.manifest", error, str(path))]
    if not isinstance(data, dict):
        return None, [_issue("fixture.manifest", "fixture manifest must be an object", str(path))]
    return data, []


def test_schema_fixtures() -> dict[str, Any]:
    manifest, errors = _fixture_manifest()
    results: list[dict[str, Any]] = []
    if manifest is None:
        return {"ok": False, "fixtures": results, "errors": errors}
    entries = manifest.get("fixtures")
    if not isinstance(entries, list) or not entries:
        errors.append(_issue("fixture.manifest.entries", "fixtures must be a non-empty array", "$.fixtures"))
        return {"ok": False, "fixtures": results, "errors": errors}
    for index, entry in enumerate(entries):
        entry_path = _path_join("$.fixtures", index)
        if not isinstance(entry, dict):
            errors.append(_issue("fixture.manifest.entry", "fixture entry must be an object", entry_path))
            continue
        relative = entry.get("path")
        expected = entry.get("expected")
        if not isinstance(relative, str) or not relative:
            errors.append(_issue("fixture.manifest.path", "fixture path is required", _path_join(entry_path, "path")))
            continue
        fixture_path = (FIXTURES_DIR / relative).resolve()
        if FIXTURES_DIR.resolve() not in fixture_path.parents:
            errors.append(_issue("fixture.manifest.path", "fixture path escapes the fixture directory", _path_join(entry_path, "path")))
            continue
        data, error = _load_json(fixture_path)
        if error:
            errors.append(_issue("fixture.file", f"{relative}: {error}", _path_join(entry_path, "path")))
            continue
        report = validate_platform_graph(data, source_path=fixture_path)
        observed = "valid" if report["ok"] else "invalid"
        passed = observed == expected
        results.append({"path": relative, "expected": expected, "observed": observed, "passed": passed, "report": report})
        if not passed:
            errors.append(_issue("fixture.expectation", f"{relative}: expected {expected}, observed {observed}", _path_join(entry_path, "expected")))
    return {"ok": not errors and all(item["passed"] for item in results), "fixtures": results, "errors": errors}


def _parse_plan_index() -> tuple[list[dict[str, str]], list[dict[str, Any]]]:
    errors: list[dict[str, Any]] = []
    entries: list[dict[str, str]] = []
    if not PLAN_INDEX_PATH.exists():
        return entries, [_issue("plan.index", "PLAN_INDEX.md is missing")]
    pattern = re.compile(r"^\|\s*(P\d+)\s*\|\s*\[[^]]+\]\(([^)]+)\)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*$")
    for line in PLAN_INDEX_PATH.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            entries.append(
                {
                    "id": match.group(1).strip(),
                    "path": match.group(2).strip(),
                    "depends_on": match.group(3).strip(),
                    "status": match.group(4).strip(),
                    "evidence": match.group(5).strip(),
                }
            )
    if not entries:
        errors.append(_issue("plan.index", "PLAN_INDEX.md contains no plan table entries"))
    return entries, errors


def _resolve_plan_path(value: Any, *, base: Path = REPO_ROOT) -> Path:
    """Resolve either a repository-relative or PLAN_INDEX-relative plan path."""

    path = Path(str(value or ""))
    if path.is_absolute():
        return path.resolve()
    candidates = [
        (base / path).resolve(),
        (REPO_ROOT / path).resolve(),
        (PLAN_INDEX_PATH.parent / path).resolve(),
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def _normalise_plan_dependencies(value: Any) -> list[str]:
    text = str(value or "").strip().strip("`")
    if not text or text.lower() in {"none", "-", "—", "–"}:
        return []
    result: list[str] = []
    for token in re.split(r"[,;]", text):
        token = token.strip().upper()
        range_match = re.fullmatch(r"P(\d+)\s*[-–—]\s*P(\d+)", token)
        if range_match:
            first, last = int(range_match.group(1)), int(range_match.group(2))
            result.extend(f"P{number:02d}" for number in range(first, last + 1))
        elif re.fullmatch(r"P\d+", token):
            result.append(token)
    return result


def _manifest_schema_errors(manifest: Any) -> list[dict[str, Any]]:
    schema, error = _load_json(PLAN_MANIFEST_SCHEMA_PATH)
    if error or not isinstance(schema, dict):
        return [_issue("plan.schema", error or "plan manifest schema is not an object")]
    return validate_json_instance(
        manifest,
        schema,
        root=schema,
        source_path=PLAN_MANIFEST_SCHEMA_PATH,
        cache={PLAN_MANIFEST_SCHEMA_PATH.resolve(): schema},
    )


def _find_cycles(entries: dict[str, dict[str, Any]]) -> list[list[str]]:
    cycles: list[list[str]] = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            cycles.append(visiting[visiting.index(node):] + [node])
            return
        if node in visited:
            return
        visiting.append(node)
        for dependency in entries[node].get("depends_on", []):
            if dependency in entries:
                visit(dependency)
        visiting.pop()
        visited.add(node)

    for node in entries:
        visit(node)
    return cycles


def validate_plan_manifest(manifest_path: Path = PLAN_MANIFEST_PATH) -> dict[str, Any]:
    manifest, error = _load_json(manifest_path)
    if error:
        return {"ok": False, "manifest": str(manifest_path), "errors": [_issue("plan.manifest", error)]}
    errors = _manifest_schema_errors(manifest)
    if not isinstance(manifest, dict):
        errors.append(_issue("plan.manifest", "manifest must be an object"))
        return {"ok": False, "manifest": str(manifest_path), "errors": errors}
    index_entries, index_errors = _parse_plan_index()
    errors.extend(index_errors)
    plans = manifest.get("plans", [])
    by_id: dict[str, dict[str, Any]] = {}
    for index, plan in enumerate(plans if isinstance(plans, list) else []):
        if not isinstance(plan, dict):
            errors.append(_issue("plan.entry", "plan must be an object", _path_join("$.plans", index)))
            continue
        plan_id = plan.get("id")
        if not isinstance(plan_id, str):
            continue
        if plan_id in by_id:
            errors.append(_issue("plan.duplicate_id", f"duplicate plan id {plan_id!r}", _path_join(_path_join("$.plans", index), "id")))
        by_id[plan_id] = plan
        if not isinstance(plan.get("path"), str) or not _resolve_plan_path(plan.get("path")).exists():
            errors.append(_issue("plan.path", f"plan file is missing for {plan_id}", _path_join(_path_join("$.plans", index), "path")))
        if not isinstance(plan.get("verification_commands"), list) or not plan.get("verification_commands"):
            errors.append(_issue("plan.commands", f"{plan_id} must define verification commands", _path_join(_path_join("$.plans", index), "verification_commands")))
        if not isinstance(plan.get("required_evidence"), list) or not plan.get("required_evidence"):
            errors.append(_issue("plan.evidence", f"{plan_id} must define required evidence references", _path_join(_path_join("$.plans", index), "required_evidence")))
        for dependency in plan.get("depends_on", []) if isinstance(plan.get("depends_on"), list) else []:
            if dependency not in by_id and dependency not in {item.get("id") for item in plans if isinstance(item, dict)}:
                errors.append(_issue("plan.dependency_missing", f"{plan_id} depends on unknown plan {dependency!r}"))
            if dependency == plan_id:
                errors.append(_issue("plan.dependency_self", f"{plan_id} cannot depend on itself"))

    manifest_ids = set(by_id)
    index_id_counts: dict[str, int] = {}
    for item in index_entries:
        index_id_counts[item["id"]] = index_id_counts.get(item["id"], 0) + 1
    for plan_id, count in index_id_counts.items():
        if count > 1:
            errors.append(_issue("plan.duplicate_id", f"duplicate plan id {plan_id!r} in PLAN_INDEX.md"))
    index_ids = set(index_id_counts)
    if manifest_ids != index_ids:
        errors.append(_issue("plan.coverage", f"manifest/index mismatch: manifest={sorted(manifest_ids)}, index={sorted(index_ids)}"))
    for item in index_entries:
        plan = by_id.get(item["id"])
        if plan is None:
            continue
        manifest_plan_path = _resolve_plan_path(plan.get("path"))
        index_path = _resolve_plan_path(item["path"], base=PLAN_INDEX_PATH.parent)
        if manifest_plan_path != index_path:
            errors.append(_issue("plan.index_path", f"{item['id']} path does not match PLAN_INDEX.md"))
        if plan.get("status") != item["status"]:
            errors.append(_issue("plan.index_status", f"{item['id']} status does not match PLAN_INDEX.md"))
        expected_dependencies = _normalise_plan_dependencies(item.get("depends_on"))
        actual_dependencies = [str(value).strip().upper() for value in plan.get("depends_on", [])]
        if actual_dependencies != expected_dependencies:
            errors.append(_issue("plan.index_dependencies", f"{item['id']} dependencies do not match PLAN_INDEX.md"))
    for cycle in _find_cycles(by_id):
        errors.append(_issue("plan.dependency_cycle", "dependency cycle: " + " -> ".join(cycle)))
    return {
        "ok": not errors,
        "manifest": str(manifest_path.relative_to(REPO_ROOT)).replace("\\", "/") if manifest_path.is_relative_to(REPO_ROOT) else str(manifest_path),
        "version": manifest.get("version"),
        "plans": len(plans) if isinstance(plans, list) else 0,
        "plan_ids": sorted(manifest_ids),
        "errors": errors,
    }


def _command_argv(command: str) -> list[str]:
    # Plan commands use shell-neutral quoting.  POSIX tokenization is also the
    # least surprising behavior for PowerShell-hosted Python invocations such
    # as `python -c "..."`.
    tokens = shlex.split(command, posix=True)
    if not tokens:
        return []
    if tokens[0].lower() in {"python", "python3"}:
        tokens[0] = sys.executable
    return tokens


def _run_command(command: str, timeout: int = 300) -> dict[str, Any]:
    argv = _command_argv(command)
    started = time.monotonic()
    if not argv:
        return {"command": command, "argv": [], "returncode": 2, "stdout": "", "stderr": "empty command", "duration_seconds": 0.0}
    try:
        completed = subprocess.run(
            argv,
            cwd=str(REPO_ROOT),
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        return {
            "command": command,
            "argv": argv,
            "returncode": completed.returncode,
            "stdout": stdout,
            "stderr": stderr,
            "stdout_sha256": hashlib.sha256(stdout.encode("utf-8")).hexdigest(),
            "stderr_sha256": hashlib.sha256(stderr.encode("utf-8")).hexdigest(),
            "stdout_bytes": len(stdout.encode("utf-8")),
            "stderr_bytes": len(stderr.encode("utf-8")),
            "duration_seconds": round(time.monotonic() - started, 3),
        }
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        stdout = getattr(exc, "stdout", "") or ""
        stderr = str(exc)
        return {
            "command": command,
            "argv": argv,
            "returncode": 127 if isinstance(exc, FileNotFoundError) else 124,
            "stdout": stdout,
            "stderr": stderr,
            "stdout_sha256": hashlib.sha256(str(stdout).encode("utf-8")).hexdigest(),
            "stderr_sha256": hashlib.sha256(stderr.encode("utf-8")).hexdigest(),
            "stdout_bytes": len(str(stdout).encode("utf-8")),
            "stderr_bytes": len(stderr.encode("utf-8")),
            "duration_seconds": round(time.monotonic() - started, 3),
        }


def _is_self_verify(command: str, plan_id: str) -> bool:
    normalized = command.replace("\\", "/")
    return bool(re.search(rf"\bplan\s+verify\s+{re.escape(plan_id)}\s*$", normalized))


def _environment_fingerprint() -> dict[str, Any]:
    commit = "unknown"
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=str(REPO_ROOT), text=True, capture_output=True, check=False
        ).stdout.strip() or "unknown"
    except OSError:
        pass
    fingerprint = {
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "repository_commit": commit,
    }
    fingerprint["sha256"] = hashlib.sha256(
        json.dumps(fingerprint, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return fingerprint


def _hash_file(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _relative_repo_path(path: Path) -> str:
    return str(path.resolve().relative_to(REPO_ROOT)).replace("\\", "/")


def _write_plan_evidence_logs(
    evidence_dir: Path,
    command_reports: list[dict[str, Any]],
) -> None:
    """Persist command output without making the JSON result self-referential."""

    for index, report in enumerate(command_reports, 1):
        if report.get("skipped"):
            continue
        command_id = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(report.get("command", "command")))[:80]
        log_path = evidence_dir / f"command-{index:02d}-{command_id}.log"
        stdout = str(report.get("stdout", "") or "")
        stderr = str(report.get("stderr", "") or "")
        log_path.write_text(
            f"$ {report.get('command', '')}\n\n[stdout]\n{stdout}\n[stderr]\n{stderr}",
            encoding="utf-8",
        )
        report["log"] = _relative_repo_path(log_path)


def _write_plan_bundle_metadata(
    evidence_dir: Path,
    plan_id: str,
    verified_commit: str,
    command_reports: list[dict[str, Any]],
) -> None:
    unit_test_lines = []
    for report in command_reports:
        if "unittest" not in str(report.get("command", "")):
            continue
        unit_test_lines.append(
            f"$ {report.get('command', '')}\n"
            f"returncode={report.get('returncode')}\n"
            f"stdout_sha256={report.get('stdout_sha256', '')}\n"
            f"stderr_sha256={report.get('stderr_sha256', '')}\n"
            f"{report.get('stdout', '')}{report.get('stderr', '')}"
        )
    (evidence_dir / "unit-test.log").write_text(
        "\n\n".join(unit_test_lines) + ("\n" if unit_test_lines else ""),
        encoding="utf-8",
    )

    bundle_payload = {
        "artifact": "treatcode-plan-verification",
        "plan_id": plan_id,
        "commit": verified_commit,
        "command_hashes": [
            {
                "command": report.get("command"),
                "returncode": report.get("returncode"),
                "stdout_sha256": report.get("stdout_sha256"),
                "stderr_sha256": report.get("stderr_sha256"),
            }
            for report in command_reports
        ],
    }
    bundle_hash = hashlib.sha256(
        json.dumps(bundle_payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    artifact_id = os.environ.get("GITHUB_RUN_ID") or f"local:{verified_commit}"
    (evidence_dir / "ci-artifact.json").write_text(
        json.dumps(
            {
                "artifact_id": artifact_id,
                "artifact_name": "treatcode-plan-verification",
                "content_hash": "sha256:" + bundle_hash,
                "bundle": bundle_payload,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def verify_plan(plan_id: str, manifest_path: Path = PLAN_MANIFEST_PATH) -> dict[str, Any]:
    manifest, load_error = _load_json(manifest_path)
    if load_error or not isinstance(manifest, dict):
        return {"ok": False, "complete": False, "plan_id": plan_id, "errors": [_issue("plan.manifest", load_error or "manifest must be an object")]}
    manifest_report = validate_plan_manifest(manifest_path)
    errors: list[dict[str, Any]] = []
    if not manifest_report["ok"]:
        errors.extend(manifest_report["errors"])
    plans = manifest.get("plans", [])
    plan = next((item for item in plans if isinstance(item, dict) and item.get("id") == plan_id), None)
    if plan is None:
        errors.append(_issue("plan.not_found", f"unknown plan {plan_id!r}"))
        return {"ok": False, "complete": False, "plan_id": plan_id, "errors": errors}

    dependency_commits: dict[str, str | None] = {}
    for dependency in plan.get("depends_on", []):
        dependency_plan = next((item for item in plans if isinstance(item, dict) and item.get("id") == dependency), None)
        if not isinstance(dependency_plan, dict):
            continue
        dependency_status = dependency_plan.get("status")
        dependency_evidence = dependency_plan.get("completion_evidence")
        dependency_commits[dependency] = dependency_plan.get("verified_commit")
        if dependency_status != "complete":
            errors.append(_issue("plan.dependency_incomplete", f"dependency {dependency} is {dependency_status!r}, not complete"))
        if dependency_status == "complete" and (not isinstance(dependency_evidence, str) or not dependency_evidence):
            errors.append(_issue("plan.dependency_evidence", f"completed dependency {dependency} has no completion evidence"))

    evidence_dir = EVIDENCE_ROOT / plan_id
    evidence_dir.mkdir(parents=True, exist_ok=True)
    result_path = evidence_dir / "result.json"

    command_reports: list[dict[str, Any]] = []
    for command in plan.get("verification_commands", []):
        if _is_self_verify(str(command), plan_id):
            command_reports.append(
                {
                    "command": command,
                    "skipped": True,
                    "reason": "self-verification command",
                    "returncode": 0,
                }
            )
            continue
        result = _run_command(str(command))
        command_reports.append(result)
        if result.get("returncode") != 0:
            errors.append(_issue("plan.command_failed", f"verification command exited {result.get('returncode')}: {command}"))

    _write_plan_evidence_logs(evidence_dir, command_reports)

    status = plan.get("status")
    approvals = plan.get("approvals", [])
    approval_roles = {item.get("role") for item in approvals if isinstance(item, dict) and item.get("decision") == "approved"}
    required_roles = set(plan.get("required_approvals", []))
    missing_approvals = sorted(required_roles - approval_roles)
    if missing_approvals:
        errors.append(_issue("plan.approval_missing", f"missing required approvals: {', '.join(missing_approvals)}"))

    if status != "complete":
        errors.append(_issue("plan.status_incomplete", f"plan {plan_id} is {status!r}, not complete"))
    if status == "complete":
        if not isinstance(plan.get("completion_evidence"), str) or not plan.get("completion_evidence"):
            errors.append(_issue("plan.completion_evidence", f"complete plan {plan_id} has no completion evidence"))
        if not isinstance(plan.get("verified_commit"), str) or not plan.get("verified_commit"):
            errors.append(_issue("plan.verified_commit", f"complete plan {plan_id} has no verified commit"))

    environment = _environment_fingerprint()
    verified_commit = environment["repository_commit"]
    _write_plan_bundle_metadata(evidence_dir, plan_id, verified_commit, command_reports)

    evidence_records: list[dict[str, Any]] = []
    evidence_hashes: dict[str, str] = {}
    for evidence in plan.get("required_evidence", []):
        reference = str(evidence).strip()
        if "://" in reference:
            evidence_records.append({"reference": reference, "exists": False, "sha256": None})
            errors.append(_issue("plan.evidence_missing", f"required evidence is not a local artifact: {reference}"))
            continue
        evidence_path = _resolve_plan_path(reference)
        is_result = evidence_path.resolve() == result_path.resolve()
        digest = None if is_result else _hash_file(evidence_path)
        evidence_records.append(
            {
                "reference": reference,
                "path": _relative_repo_path(evidence_path) if evidence_path.is_relative_to(REPO_ROOT) else str(evidence_path),
                "exists": bool(digest or is_result),
                "sha256": digest,
                "self_reference": is_result,
            }
        )
        if is_result:
            continue
        if digest:
            evidence_hashes[reference] = digest
        else:
            errors.append(_issue("plan.evidence_missing", f"required evidence file is missing: {reference}"))

    machine_ok = not errors
    complete = bool(machine_ok and status == "complete" and not missing_approvals)
    result = {
        "schema": "treatcode.plan-verification-result.v1",
        "ok": machine_ok,
        "complete": complete,
        "machine_gates_passed": machine_ok and not any(
            item["code"].startswith("plan.approval") or item["code"] == "plan.status_incomplete"
            for item in errors
        ),
        "plan_id": plan_id,
        "plan_version": plan.get("version"),
        "plan_status": status,
        "verified_commit": verified_commit,
        "dependency_commits": dependency_commits,
        "verifier": "tools/treatcode_platform.py",
        "verification_commands": command_reports,
        "command_exit_codes": {
            item["command"]: item.get("returncode") for item in command_reports
        },
        "environment_fingerprint": environment,
        "evidence_artifact_paths": [str(result_path.relative_to(REPO_ROOT)).replace("\\", "/")],
        "evidence_artifact": str(result_path.relative_to(REPO_ROOT)).replace("\\", "/"),
        "evidence_artifact_hashes": evidence_hashes,
        "evidence_records": evidence_records,
        "human_approvals": approvals,
        "missing_approvals": missing_approvals,
        "verification_date": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "errors": errors,
    }
    # The normalized content hash is stable while the actual file remains a
    # self-contained evidence record. The result file itself is excluded from
    # evidence_artifact_hashes to avoid an impossible self-reference.
    result["content_hash"] = "sha256:" + hashlib.sha256(
        json.dumps(result, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def _print_report(report: dict[str, Any], title: str, json_output: bool) -> int:
    if json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(title)
        if report.get("ok"):
            print(f"[ok] {title.lower()}")
        else:
            print(f"[fail] {len(report.get('errors', []))} validation error(s)")
            for error in report.get("errors", [])[:20]:
                print(f"  {error.get('code')}: {error.get('message')}")
    return 0 if report.get("ok") else 1


def _write_p01_evidence(name: str, report: dict[str, Any]) -> None:
    """Persist the reports named by P01's evidence contract."""

    output = EVIDENCE_ROOT / "P01" / name
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cmd_website(args: Any) -> int:
    action = getattr(args, "website_action", "")
    json_output = bool(getattr(args, "json", False))
    if action == "schemas_validate":
        report = validate_schema_catalog()
        _write_p01_evidence("schema-validation.json", report)
        return _print_report(report, "Trit website schemas validate", json_output)
    if action == "schemas_fixtures":
        report = test_schema_fixtures()
        _write_p01_evidence("fixture-test.json", report)
        return _print_report(report, "Trit website schemas test-fixtures", json_output)
    if action == "plans_validate":
        return _print_report(validate_plan_manifest(), "Trit website plans validate", json_output)
    if action == "plan_verify":
        report = verify_plan(str(args.plan_id))
        if json_output:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print(f"Trit website plan verify {args.plan_id}")
            if report.get("ok"):
                state = "complete" if report.get("complete") else "machine gates passed; human/dependency gates remain"
                print(f"[ok] {state}")
            else:
                print(f"[fail] {len(report.get('errors', []))} verification error(s)")
                for error in report.get("errors", [])[:20]:
                    print(f"  {error.get('code')}: {error.get('message')}")
        # A pre-completion verification is successful when all runnable machine
        # checks pass.  A plan explicitly marked complete must be fully complete.
        return 0 if report.get("ok") else 1
    raise RuntimeError(f"unknown website action {action!r}")


__all__ = [
    "cmd_website",
    "validate_json_instance",
    "validate_plan_manifest",
    "validate_platform_graph",
    "validate_schema_catalog",
    "test_schema_fixtures",
    "verify_plan",
]
