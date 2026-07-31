#!/usr/bin/env python3
"""Deterministic repository ingestion for the TreatCode platform.

The ingestion index is deliberately conservative. Git is the inventory
authority, source spans are line/column locations in the indexed working tree,
and relationship extraction is explicit about unresolved and external targets.
The module only uses the Python standard library so it can run before any
project build has completed.
"""

from __future__ import annotations

import copy
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable, Sequence


DEFAULT_REPO_ROOT = Path(__file__).resolve().parents[1]
INDEX_SCHEMA = "treatcode.repository-index.v1"
INDEX_VERSION = 1
INDEX_FILENAME = "repository-index.v1.json"
COVERAGE_FILENAME = "coverage.v1.json"
FRESHNESS_FILENAME = "freshness.v1.json"
DETERMINISM_FILENAME = "determinism.v1.json"
CONTEXT_SCHEMA = "treatcode.context-package.v1"
REPOSITORY_NAME = "trit"


LANGUAGE_BY_SUFFIX = {
    ".c": "c",
    ".cc": "cpp",
    ".cpp": "cpp",
    ".cxx": "cpp",
    ".h": "cpp",
    ".hh": "cpp",
    ".hpp": "cpp",
    ".hxx": "cpp",
    ".m": "c",
    ".py": "python",
    ".ps1": "powershell",
    ".cmd": "batch",
    ".bat": "batch",
    ".trit": "trit",
    ".tasm": "assembly",
    ".asm": "assembly",
    ".s": "assembly",
    ".S": "assembly",
    ".ts": "typescript",
    ".tsx": "typescript",
    ".js": "javascript",
    ".jsx": "javascript",
    ".json": "json",
    ".md": "markdown",
    ".markdown": "markdown",
    ".cmake": "cmake",
    ".toml": "toml",
    ".yaml": "yaml",
    ".yml": "yaml",
    ".csv": "csv",
}

PARSER_LANGUAGES = {
    "c",
    "cpp",
    "python",
    "trit",
    "assembly",
    "typescript",
    "javascript",
}

CONTROL_CALL_NAMES = {
    "if",
    "for",
    "while",
    "switch",
    "catch",
    "sizeof",
    "decltype",
    "static_cast",
    "dynamic_cast",
    "reinterpret_cast",
    "const_cast",
    "return",
    "fn",
    "def",
    "function",
    "proc",
    "match",
}

GENERATED_PREFIXES = (
    "generated/",
    "treatcode/dist/",
    "dist/",
)
DOWNLOADED_PREFIXES = (
    "bitnet_weights/model/",
    "bitnet_weights/converted_t40/",
    "qwen3.627b_weights/",
)

CPP_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]")
PY_FROM_RE = re.compile(r"^\s*from\s+([.A-Za-z_][.A-Za-z0-9_.]*)\s+import\b")
PY_IMPORT_RE = re.compile(r"^\s*import\s+([.A-Za-z_][.A-Za-z0-9_.]*)")
JS_IMPORT_RE = re.compile(
    r"\b(?:import\s+(?:[^;]*?\s+from\s+)?|require\s*\(\s*)['\"]([^'\"]+)['\"]"
)
TRIT_IMPORT_RE = re.compile(r"^\s*(?:import|include)\s+[<\"]?([^>\"\s]+)")
GENERIC_DECL_RE = re.compile(
    r"^\s*(?:(?:pub|public|export|async|static)\s+)*(?P<kind>fn|func|function|proc|def|class|struct|enum|interface|type)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
)
CPP_TYPE_RE = re.compile(r"^\s*(?:(?:template\s*<[^>]*>\s*)|(?:export\s+))*\b(?P<kind>class|struct|enum|union)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)")
CPP_DEFINE_RE = re.compile(r"^\s*#\s*define\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)")
TRIT_CONST_RE = re.compile(r"^\s*(?:pub\s+)?const\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b")
PY_DECL_RE = re.compile(r"^\s*(?P<kind>def|class|async\s+def)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)")
JS_DECL_RE = re.compile(
    r"^\s*(?:(?:export|default|async|declare|abstract)\s+)*(?P<kind>function|class|interface|type|enum)\s+(?P<name>[A-Za-z_$][A-Za-z0-9_$]*)"
)
JS_CONST_RE = re.compile(r"^\s*(?:export\s+)?(?:const|let|var)\s+(?P<name>[A-Za-z_$][A-Za-z0-9_$]*)")
CPP_FUNCTION_RE = re.compile(
    r"^\s*(?:(?:template\s*<[^>]*>\s*)|(?:(?:static|inline|constexpr|consteval|virtual|extern|friend|explicit|noexcept|public|private|protected)\s+))*"
    r"[A-Za-z_][A-Za-z0-9_:<>~*&\s,]*?\s+(?P<name>~?[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)\s*"
    r"\([^;{}]*\)\s*(?:const\b[^{}]*)?(?:\{|$)"
)
ASSEMBLY_LABEL_RE = re.compile(r"^\s*(?P<name>[A-Za-z_.$][A-Za-z0-9_.$]*):")
CALL_RE = re.compile(r"(?<![A-Za-z0-9_$])(?P<name>[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)?)\s*\(")
PATH_RE = re.compile(r"(?P<path>[A-Za-z0-9_./\\-]+\.[A-Za-z0-9_+-]+)")
MARKDOWN_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s#]+)")


def canonical_json(value: Any) -> str:
    """Return the stable JSON representation used for hashes and artifacts."""

    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _sha256_text(value: str) -> str:
    return _sha256_bytes(value.encode("utf-8"))


def _posix_path(path: Path | str) -> str:
    return str(path).replace("\\", "/")


def _normalise_relative(path: str) -> str:
    value = _posix_path(path)
    while value.startswith("./"):
        value = value[2:]
    return value


def _path_within(root: Path, relative: str) -> Path | None:
    candidate = (root / Path(relative)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        return None
    return candidate


def _run_git(repo_root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_root), *args],
        text=True,
        capture_output=True,
        check=False,
    )


def _git_commit(repo_root: Path) -> str:
    result = _run_git(repo_root, "rev-parse", "HEAD")
    commit = (result.stdout or "").strip()
    if result.returncode != 0 or not re.fullmatch(r"[0-9a-fA-F]{7,64}", commit):
        raise RuntimeError((result.stderr or "git rev-parse HEAD failed").strip())
    return commit.lower()


def _git_paths(repo_root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files", "-z"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stderr or b"git ls-files failed").decode("utf-8", errors="replace").strip())
    raw_paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    return sorted({_normalise_relative(path) for path in raw_paths if path})


def _git_untracked_paths(repo_root: Path, *, exclude_paths: set[str] | None = None) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files", "--others", "--exclude-standard", "-z"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stderr or b"git ls-files --others failed").decode("utf-8", errors="replace").strip())
    raw_paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    excluded = {_normalise_relative(path) for path in (exclude_paths or set())}
    return sorted(
        {
            normal
            for path in raw_paths
            for normal in [_normalise_relative(path)]
            if path and normal not in excluded
            and not any(normal.startswith(prefix.rstrip("/") + "/") for prefix in excluded)
            and "__pycache__/" not in normal and not path.endswith(".pyc")
            and _role_for(normal, _language_for(normal))[0] not in {"generated", "downloaded"}
        }
    )


def _git_blob(repo_root: Path, commit: str, path: str) -> bytes | None:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "show", f"{commit}:{path}"],
        capture_output=True,
        check=False,
    )
    return result.stdout if result.returncode == 0 else None


def _git_dirty_tracked_paths(repo_root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "diff", "--name-only", "--no-renames", "-z", "HEAD", "--"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stderr or b"git diff failed").decode("utf-8", errors="replace").strip())
    raw_paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    return sorted({_normalise_relative(path) for path in raw_paths if path})


def _repository_name(repo_root: Path) -> str:
    if repo_root.name.lower() == REPOSITORY_NAME:
        return REPOSITORY_NAME
    result = _run_git(repo_root, "config", "--get", "remote.origin.url")
    remote = (result.stdout or "").strip()
    if remote:
        remote = remote.rstrip("/").rsplit("/", 1)[-1]
        remote = remote.rsplit("\\", 1)[-1]
        if remote.endswith(".git"):
            remote = remote[:-4]
        if remote:
            return remote
    return repo_root.name


def _language_for(path: str) -> str:
    name = Path(path).name
    if name == "CMakeLists.txt":
        return "cmake"
    return LANGUAGE_BY_SUFFIX.get(Path(name).suffix, "binary")


def _role_for(path: str, language: str) -> tuple[str, str, bool, str]:
    lower = path.lower()
    if lower.startswith(GENERATED_PREFIXES):
        return "generated", "generated", False, "tracked generated output is indexed for provenance only"
    if lower.startswith(DOWNLOADED_PREFIXES):
        return "downloaded", "downloaded", False, "tracked downloaded/model input is not source authority"
    if lower.startswith("tests/fixtures/") or lower.startswith("tests_next/fixtures/"):
        return "fixture", "fixture", False, "fixture data is evidence input, not implementation authority"
    if lower.startswith("tests/") or lower.startswith("tests_next/") or re.search(r"(^|[/_])test[s]?([_/.-]|$)", lower):
        return "test", "source", True, "test source"
    if lower.startswith("benchmarks/") or "benchmark" in Path(lower).name:
        return "benchmark", "source", True, "benchmark source or result contract"
    if lower.startswith("docs/") or Path(lower).suffix in {".md", ".markdown"}:
        return "documentation", "source", True, "curated documentation"
    if Path(lower).suffix == ".json":
        return "manifest", "source", True, "structured project manifest or data contract"
    if language in PARSER_LANGUAGES:
        return "source", "source", True, "tracked implementation source"
    if lower.startswith("tools/"):
        return "tooling", "source", True, "repository tooling"
    if lower.startswith("treatcode/"):
        return "website", "source", True, "TreatCode website source"
    return "asset", "source", True, "tracked repository asset"


def _read_file(path: Path) -> tuple[bytes, str | None, bool]:
    raw = path.read_bytes()
    if b"\0" in raw:
        return raw, None, True
    try:
        return raw, raw.decode("utf-8"), False
    except UnicodeDecodeError:
        return raw, raw.decode("utf-8", errors="replace"), False


def _span(line: int, start_column: int, end_column: int) -> dict[str, int]:
    return {
        "start_line": max(1, int(line)),
        "start_column": max(1, int(start_column)),
        "end_line": max(1, int(line)),
        "end_column": max(max(1, int(start_column)), int(end_column)),
    }


def _source_ref(
    repository: str,
    commit: str,
    path: str,
    span: dict[str, int] | None = None,
    *,
    role: str | None = None,
    status: str = "resolved",
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "repository": repository,
        "commit": commit,
        "path": _normalise_relative(path),
        "span": copy.deepcopy(span),
        "status": status,
    }
    if role:
        result["role"] = role
    return result


def _file_id(path: str) -> str:
    return "file:" + _normalise_relative(path)


def _symbol_id(path: str, kind: str, name: str, line: int) -> str:
    return f"symbol:{_normalise_relative(path)}:{kind}:{name}:{line}"


def _relationship_id(
    relation_type: str,
    source_id: str,
    target_id: str,
    source: dict[str, Any],
    relation_key: str = "",
) -> str:
    payload = "|".join(
        [
            relation_type,
            source_id,
            target_id,
            str(source.get("path", "")),
            json.dumps(source.get("span"), sort_keys=True),
            relation_key,
        ]
    )
    return "relationship:" + hashlib.sha256(payload.encode("utf-8")).hexdigest()[:24]


def _line_for_offset(text: str, offset: int) -> tuple[int, int]:
    line = text.count("\n", 0, offset) + 1
    previous = text.rfind("\n", 0, offset)
    return line, offset - previous


def _find_span(text: str, needle: str, start: int = 0) -> tuple[dict[str, int] | None, int]:
    if not needle:
        return None, start
    offset = text.find(needle, start)
    if offset < 0:
        return None, start
    line, column = _line_for_offset(text, offset)
    return _span(line, column, column + len(needle)), offset + len(needle)


def _is_comment_or_blank(line: str, language: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return True
    if stripped.startswith("//") or stripped.startswith("#") and language not in {"python", "powershell"}:
        return True
    if stripped.startswith(";") or stripped.startswith("--"):
        return True
    return False


def _declaration_symbols(path: str, language: str, lines: Sequence[str]) -> list[dict[str, Any]]:
    symbols: list[dict[str, Any]] = []
    seen: set[tuple[str, str, int]] = set()

    def add(kind: str, name: str, line_number: int, start_column: int = 1) -> None:
        key = (kind, name, line_number)
        if key in seen:
            return
        seen.add(key)
        source_span = _span(line_number, start_column, len(lines[line_number - 1]) + 1)
        symbols.append(
            {
                "id": _symbol_id(path, kind, name, line_number),
                "name": name,
                "qualified_name": name,
                "kind": kind,
                "path": path,
                "source_span": source_span,
                "signature": lines[line_number - 1].strip()[:240],
            }
        )

    for line_number, line in enumerate(lines, 1):
        if _is_comment_or_blank(line, language):
            continue
        if language == "assembly":
            match = ASSEMBLY_LABEL_RE.match(line)
            if match:
                add("label", match.group("name"), line_number, match.start("name") + 1)
            continue
        if language == "python":
            match = PY_DECL_RE.match(line)
            if match:
                kind = "function" if "def" in match.group("kind") else "class"
                add(kind, match.group("name"), line_number, match.start("name") + 1)
            continue
        if language in {"typescript", "javascript"}:
            match = JS_DECL_RE.match(line)
            if match:
                add("type" if match.group("kind") in {"class", "interface", "type", "enum"} else "function", match.group("name"), line_number, match.start("name") + 1)
            else:
                match = JS_CONST_RE.match(line)
                if match:
                    add("constant", match.group("name"), line_number, match.start("name") + 1)
            continue
        if language == "trit":
            match = GENERIC_DECL_RE.match(line)
            if match:
                generic_kind = match.group("kind")
                kind = "function" if generic_kind in {"fn", "func", "function", "proc"} else "type"
                add(kind, match.group("name"), line_number, match.start("name") + 1)
            match = TRIT_CONST_RE.match(line)
            if match:
                add("constant", match.group("name"), line_number, match.start("name") + 1)
            continue
        match = CPP_TYPE_RE.match(line)
        if match:
            add("type", match.group("name"), line_number, match.start("name") + 1)
        match = CPP_DEFINE_RE.match(line)
        if match:
            add("constant", match.group("name"), line_number, match.start("name") + 1)
        match = CPP_FUNCTION_RE.match(line)
        if match:
            add("function", match.group("name"), line_number, match.start("name") + 1)

    return sorted(symbols, key=lambda item: (item["path"], item["source_span"]["start_line"], item["kind"], item["name"]))


def _import_mentions(path: str, language: str, lines: Sequence[str]) -> list[dict[str, Any]]:
    mentions: list[dict[str, Any]] = []
    for line_number, line in enumerate(lines, 1):
        if _is_comment_or_blank(line, language):
            continue
        match: re.Match[str] | None = None
        relation_type = "imports"
        raw = ""
        if language in {"c", "cpp"}:
            match = CPP_INCLUDE_RE.match(line)
            if match:
                relation_type = "includes"
                raw = match.group(2)
        elif language == "python":
            match = PY_FROM_RE.match(line) or PY_IMPORT_RE.match(line)
            if match:
                raw = match.group(1)
        elif language in {"typescript", "javascript"}:
            match = JS_IMPORT_RE.search(line)
            if match:
                raw = match.group(1)
        elif language == "trit":
            match = TRIT_IMPORT_RE.match(line)
            if match:
                raw = match.group(1)
        if not match or not raw:
            continue
        start = line.find(raw)
        mentions.append(
            {
                "type": relation_type,
                "raw": raw,
                "span": _span(line_number, max(1, start + 1), max(1, start + len(raw) + 1)),
            }
        )
    return mentions


def _call_mentions(path: str, language: str, lines: Sequence[str], symbols: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    if language not in PARSER_LANGUAGES or language == "assembly":
        return []
    declarations = {
        (item["source_span"]["start_line"], item["name"])
        for item in symbols
        if item["kind"] == "function"
    }
    calls: list[dict[str, Any]] = []
    for line_number, line in enumerate(lines, 1):
        if _is_comment_or_blank(line, language):
            continue
        for match in CALL_RE.finditer(line):
            name = match.group("name")
            if name in CONTROL_CALL_NAMES or (line_number, name) in declarations:
                continue
            calls.append(
                {
                    "name": name,
                    "span": _span(line_number, match.start("name") + 1, match.end("name") + 1),
                }
            )
    return calls


def _resolve_import(raw: str, source_path: str, tracked: set[str], repo_root: Path) -> tuple[str | None, str]:
    value = raw.replace("\\", "/")
    source_parent = Path(source_path).parent
    candidates: list[str] = []
    if value.startswith("."):
        candidates.append(_normalise_relative(str(source_parent / value)))
    elif "/" in value or value.endswith((".h", ".hpp", ".cpp", ".trit", ".py", ".ts", ".tsx", ".js", ".jsx")):
        candidates.extend([_normalise_relative(value), _normalise_relative(str(source_parent / value))])
    else:
        dotted = value.replace(".", "/")
        candidates.extend([dotted, dotted + ".py", dotted + ".ts", dotted + ".js", dotted + ".trit", dotted + ".h"])
    expanded: list[str] = []
    for candidate in candidates:
        expanded.append(candidate)
        if not Path(candidate).suffix:
            for suffix in (".h", ".hpp", ".c", ".cpp", ".trit", ".py", ".ts", ".tsx", ".js", ".jsx"):
                expanded.append(candidate + suffix)
            expanded.append(candidate + "/__init__.py")
            expanded.append(candidate + "/index.ts")
    for candidate in expanded:
        if candidate in tracked:
            return candidate, "resolved"
        resolved = _path_within(repo_root, candidate)
        if resolved and resolved.exists():
            normal = _normalise_relative(str(resolved.relative_to(repo_root.resolve())))
            if normal in tracked:
                return normal, "resolved"
    if raw.startswith("<") or raw in {"iostream", "vector", "string", "array", "filesystem", "chrono", "cstdint"}:
        return None, "external"
    return None, "unresolved"


def _resolve_record_reference(value: str, record_entities: dict[str, str], tracked: set[str]) -> tuple[str | None, str]:
    if value in record_entities:
        return record_entities[value], "resolved"
    normal = _normalise_relative(value)
    if normal in tracked:
        return _file_id(normal), "resolved"
    return None, "unresolved"


def _path_mentions(text: str, tracked: set[str]) -> list[tuple[str, dict[str, int]]]:
    mentions: list[tuple[str, dict[str, int]]] = []
    seen: set[tuple[str, int, int]] = set()
    candidates: set[str] = set()
    for match in PATH_RE.finditer(text):
        candidates.add(_normalise_relative(match.group("path")))
    for match in MARKDOWN_LINK_RE.finditer(text):
        value = match.group(1).split("#", 1)[0]
        candidates.add(_normalise_relative(value))
    for candidate in sorted(candidates):
        if candidate not in tracked:
            continue
        start = 0
        while True:
            offset = text.find(candidate, start)
            if offset < 0:
                break
            line, column = _line_for_offset(text, offset)
            key = (candidate, line, column)
            if key not in seen:
                seen.add(key)
                mentions.append((candidate, _span(line, column, column + len(candidate))))
            start = offset + len(candidate)
    return mentions


def _manifest_like(path: str) -> bool:
    name = Path(path).name.lower()
    return path.lower().endswith(".json") and (
        "manifest" in name
        or "schema" in name
        or name in {"roadmap_status.json", "stack_coverage_report.json", "benchmark_results.json"}
    )


def _record_kind(key: str, path: str) -> str:
    lower = key.lower()
    mapping = {
        "layers": "layer",
        "capabilities": "capability",
        "contracts": "contract",
        "decisions": "decision",
        "known_gaps": "gap",
        "gaps": "gap",
        "releases": "release",
        "coverage": "coverage",
        "suites": "test_suite",
        "tests": "test",
        "benchmarks": "benchmark",
        "apps": "app",
        "fixtures": "fixture",
        "plans": "plan",
    }
    if lower in mapping:
        return mapping[lower]
    if "decision" in path.lower():
        return "decision"
    if "benchmark" in path.lower():
        return "benchmark"
    if "test" in path.lower():
        return "test"
    return "manifest_record"


def _record_identifier(record: dict[str, Any], key: str, index: int, path: str) -> str | None:
    for candidate in ("id", "stack_id", "name", "title"):
        value = record.get(candidate)
        if isinstance(value, str) and value.strip():
            if candidate in {"name", "title"}:
                return f"{_record_kind(key, path)}:{value.strip()}"
            return value.strip()
    return f"{_record_kind(key, path)}:{Path(path).stem}:{index}"


def _normalise_manifest_record(
    value: Any,
    *,
    text: str,
    cursor: int,
    repository: str,
    commit: str,
) -> tuple[Any, int]:
    """Copy manifest data while adding spans to embedded source references."""

    if isinstance(value, list):
        output: list[Any] = []
        for item in value:
            normal, cursor = _normalise_manifest_record(item, text=text, cursor=cursor, repository=repository, commit=commit)
            output.append(normal)
        return output, cursor
    if not isinstance(value, dict):
        return value, cursor
    output: dict[str, Any] = {}
    for key, item in value.items():
        if key == "source_refs" and isinstance(item, list):
            refs: list[Any] = []
            for ref in item:
                if not isinstance(ref, dict):
                    refs.append(ref)
                    continue
                path = ref.get("path")
                span = None
                if isinstance(path, str):
                    span, cursor = _find_span(text, path, cursor)
                normal_ref = dict(ref)
                normal_ref["repository"] = str(ref.get("repository") or repository)
                normal_ref["commit"] = str(ref.get("commit") or commit)
                normal_ref["span"] = span
                refs.append(normal_ref)
            output[key] = refs
        else:
            normal, cursor = _normalise_manifest_record(item, text=text, cursor=cursor, repository=repository, commit=commit)
            output[key] = normal
    return output, cursor


def _annotate_manifest_span_paths(value: Any, source_path: str) -> None:
    if isinstance(value, dict):
        if {"repository", "commit", "path", "span"}.issubset(value) and value.get("span") is not None:
            value["span_path"] = source_path
        for child in value.values():
            _annotate_manifest_span_paths(child, source_path)
    elif isinstance(value, list):
        for child in value:
            _annotate_manifest_span_paths(child, source_path)


def _json_entities(
    path: str,
    text: str,
    repository: str,
    commit: str,
) -> list[dict[str, Any]]:
    if not _manifest_like(path):
        return []
    try:
        document = json.loads(text)
    except json.JSONDecodeError:
        return []
    if not isinstance(document, dict):
        return []
    entities: list[dict[str, Any]] = []
    used_ids: set[str] = set()
    for key, values in document.items():
        if not isinstance(values, list):
            continue
        for index, record in enumerate(values):
            if not isinstance(record, dict):
                continue
            record_id = _record_identifier(record, key, index, path)
            if not record_id:
                continue
            entity_kind = _record_kind(key, path)
            entity_id = "record:" + entity_kind + ":" + record_id
            if entity_id in used_ids:
                entity_id += ":" + _sha256_text(f"{path}:{key}:{index}")[7:19]
            used_ids.add(entity_id)
            needle = str(record.get("id") or record.get("stack_id") or record.get("name") or record.get("title") or key)
            source_span, _ = _find_span(text, needle)
            normal_record, _ = _normalise_manifest_record(record, text=text, cursor=0, repository=repository, commit=commit)
            _annotate_manifest_span_paths(normal_record, path)
            entities.append(
                {
                    "id": entity_id,
                    "record_id": record_id,
                    "record_key": key,
                    "kind": _record_kind(key, path),
                    "path": path,
                    "source": _source_ref(repository, commit, path, source_span, role="manifest_record"),
                    "authority": "manifest",
                    "data": normal_record,
                }
            )
    return entities


def _stable_sort_index(index: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(index)
    result["files"] = sorted(result.get("files", []), key=lambda item: item.get("path", ""))
    result["symbols"] = sorted(
        result.get("symbols", []),
        key=lambda item: (
            item.get("path", ""),
            item.get("source_span", {}).get("start_line", 0),
            item.get("kind", ""),
            item.get("name", ""),
        ),
    )
    result["entities"] = sorted(result.get("entities", []), key=lambda item: item.get("id", ""))
    result["relationships"] = sorted(
        result.get("relationships", []),
        key=lambda item: (
            item.get("type", ""),
            item.get("from", ""),
            item.get("to", ""),
            item.get("source", {}).get("path", ""),
            json.dumps(item.get("source", {}).get("span") or {}, sort_keys=True),
            item.get("id", ""),
        ),
    )
    result["excluded_files"] = sorted(result.get("excluded_files", []), key=lambda item: item.get("path", ""))
    return result


def _add_relationship(
    relationships: list[dict[str, Any]],
    seen: set[tuple[Any, ...]],
    *,
    relation_type: str,
    source_id: str,
    target_id: str,
    source: dict[str, Any],
    resolution: str = "resolved",
    relation_key: str = "",
    target_path: str | None = None,
    target_source: dict[str, Any] | None = None,
    metadata: dict[str, Any] | None = None,
) -> None:
    span = source.get("span") or {}
    key = (
        relation_type,
        source_id,
        target_id,
        source.get("path"),
        span.get("start_line"),
        span.get("start_column"),
        relation_key,
    )
    if key in seen:
        return
    seen.add(key)
    record: dict[str, Any] = {
        "id": _relationship_id(relation_type, source_id, target_id, source, relation_key),
        "type": relation_type,
        "from": source_id,
        "to": target_id,
        "resolution": resolution,
        "source": copy.deepcopy(source),
    }
    if target_path:
        record["target_path"] = target_path
    if target_source:
        record["target_source"] = copy.deepcopy(target_source)
    if metadata:
        record["metadata"] = copy.deepcopy(metadata)
    relationships.append(record)


def _record_data_references(value: Any, prefix: str = "") -> Iterable[tuple[str, str]]:
    if isinstance(value, dict):
        for key, child in value.items():
            location = f"{prefix}.{key}" if prefix else key
            if isinstance(child, list):
                for item in child:
                    if isinstance(item, str):
                        yield location, item
                    else:
                        yield from _record_data_references(item, location)
            elif isinstance(child, str):
                yield location, child
            else:
                yield from _record_data_references(child, location)
    elif isinstance(value, list):
        for item in value:
            yield from _record_data_references(item, prefix)


def _build_relationships(
    *,
    files: list[dict[str, Any]],
    symbols: list[dict[str, Any]],
    entities: list[dict[str, Any]],
    parsed: dict[str, dict[str, Any]],
    tracked: set[str],
    repository: str,
    commit: str,
) -> list[dict[str, Any]]:
    relationships: list[dict[str, Any]] = []
    seen: set[tuple[Any, ...]] = set()
    record_entities: dict[str, str] = {}
    for item in entities:
        record_id = str(item["record_id"])
        entity_id = str(item["id"])
        record_entities.setdefault(record_id, entity_id)
        record_entities.setdefault(f"{item.get('kind')}:{record_id}", entity_id)
        record_entities.setdefault(entity_id, entity_id)
    symbols_by_name: dict[str, list[dict[str, Any]]] = defaultdict(list)
    symbols_by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for symbol in symbols:
        symbols_by_name[str(symbol["name"])].append(symbol)
        if str(symbol["qualified_name"]) != str(symbol["name"]):
            symbols_by_name[str(symbol["qualified_name"])].append(symbol)
        symbols_by_path[str(symbol["path"])].append(symbol)

    for path, details in parsed.items():
        file_record = next(item for item in files if item["path"] == path)
        for mention in details.get("imports", []):
            target_path, resolution = _resolve_import(str(mention["raw"]), path, tracked, Path(details["repo_root"]))
            target_id = _file_id(target_path) if target_path else f"external:{mention['raw']}" if resolution == "external" else f"unresolved:import:{mention['raw']}"
            _add_relationship(
                relationships,
                seen,
                relation_type=str(mention["type"]),
                source_id=_file_id(path),
                target_id=target_id,
                source=_source_ref(repository, commit, path, mention["span"], role="import"),
                resolution=resolution,
                relation_key=str(mention["raw"]),
                target_path=target_path,
            )

        path_symbols = symbols_by_path.get(path, [])
        for call in details.get("calls", []):
            name = str(call["name"])
            candidates = symbols_by_name.get(name, [])
            same_file = [item for item in candidates if item["path"] == path]
            target = same_file[0] if len(same_file) == 1 else candidates[0] if len(candidates) == 1 else None
            owner_candidates = [
                item
                for item in path_symbols
                if item["kind"] == "function" and item["source_span"]["start_line"] <= call["span"]["start_line"]
            ]
            owner = max(owner_candidates, key=lambda item: item["source_span"]["start_line"]) if owner_candidates else None
            source_id = str(owner["id"]) if owner else _file_id(path)
            if target:
                target_id = str(target["id"])
                resolution = "resolved"
                target_source = _source_ref(repository, commit, str(target["path"]), target["source_span"], role="symbol")
                target_path = str(target["path"])
            else:
                target_id = "unresolved:symbol:" + name
                resolution = "unresolved"
                target_source = None
                target_path = None
            _add_relationship(
                relationships,
                seen,
                relation_type="calls",
                source_id=source_id,
                target_id=target_id,
                source=_source_ref(repository, commit, path, call["span"], role="call"),
                resolution=resolution,
                relation_key=name,
                target_path=target_path,
                target_source=target_source,
            )

    for entity in entities:
        entity_source = entity["source"]
        _add_relationship(
            relationships,
            seen,
            relation_type="manifest",
            source_id=_file_id(str(entity["path"])),
            target_id=str(entity["id"]),
            source=entity_source,
            relation_key="record",
        )
        for location, value in _record_data_references(entity.get("data")):
            target_id, resolution = _resolve_record_reference(value, record_entities, tracked)
            if target_id:
                relation_type = "references"
                if location.endswith("test_refs") or ".test_refs" in location:
                    relation_type = "tests"
                elif location.endswith("benchmark_refs") or ".benchmark_refs" in location:
                    relation_type = "benchmarks"
                elif location.endswith("decision_ids") or ".decision_ids" in location:
                    relation_type = "decisions"
                elif location.endswith("contract_ids") or ".contract_ids" in location:
                    relation_type = "contracts"
                elif location.endswith("capability_ids") or ".capability_ids" in location:
                    relation_type = "capabilities"
                elif location.endswith("gap_refs") or ".gap_refs" in location:
                    relation_type = "gaps"
                elif location.endswith("release_refs") or ".release_refs" in location:
                    relation_type = "releases"
                elif location.endswith("depends_on") or ".depends_on" in location:
                    relation_type = "depends_on"
                if "source_refs" not in location:
                    _add_relationship(
                        relationships,
                        seen,
                        relation_type=relation_type,
                        source_id=str(entity["id"]),
                        target_id=target_id,
                        source=entity_source,
                        relation_key=location + ":" + value,
                        target_path=value if value in tracked else None,
                    )
        data = entity.get("data")
        for ref in data.get("source_refs", []) if isinstance(data, dict) else []:
            if not isinstance(ref, dict) or not isinstance(ref.get("path"), str):
                continue
            path_value = _normalise_relative(str(ref["path"]))
            target_id = _file_id(path_value) if path_value in tracked else f"unresolved:path:{path_value}"
            resolution = "resolved" if path_value in tracked else "unresolved"
            ref_span = ref.get("span") if isinstance(ref.get("span"), dict) else entity_source.get("span")
            source = _source_ref(repository, commit, str(entity["path"]), ref_span, role="manifest_source_ref")
            _add_relationship(
                relationships,
                seen,
                relation_type="manifest",
                source_id=str(entity["id"]),
                target_id=target_id,
                source=source,
                resolution=resolution,
                relation_key="source_ref:" + path_value,
                target_path=path_value if path_value in tracked else None,
            )

    for path, details in parsed.items():
        file_record = next(item for item in files if item["path"] == path)
        role = str(file_record["role"])
        if role not in {"manifest", "test", "benchmark", "documentation"}:
            continue
        relation_type = {
            "manifest": "manifest",
            "test": "tests",
            "benchmark": "benchmarks",
            "documentation": "documents",
        }[role]
        text = str(details.get("text", ""))
        for target_path, span in _path_mentions(text, tracked):
            if target_path == path:
                continue
            _add_relationship(
                relationships,
                seen,
                relation_type=relation_type,
                source_id=_file_id(path),
                target_id=_file_id(target_path),
                source=_source_ref(repository, commit, path, span, role=role + "_reference"),
                relation_key=target_path,
                target_path=target_path,
            )
        for relationship in list(relationships):
            if relationship.get("from") != _file_id(path) or relationship.get("type") not in {"includes", "imports"}:
                continue
            target = str(relationship.get("to", ""))
            if target.startswith("file:"):
                _add_relationship(
                    relationships,
                    seen,
                    relation_type=relation_type,
                    source_id=_file_id(path),
                    target_id=target,
                    source=relationship["source"],
                    relation_key="derived:" + target,
                    target_path=relationship.get("target_path"),
                    metadata={"derived_from": relationship["id"]},
                )

    return sorted(
        relationships,
        key=lambda item: (
            item.get("type", ""),
            item.get("from", ""),
            item.get("to", ""),
            item.get("source", {}).get("path", ""),
            json.dumps(item.get("source", {}).get("span") or {}, sort_keys=True),
            item.get("id", ""),
        ),
    )


def _coverage(index: dict[str, Any]) -> dict[str, Any]:
    files = index.get("files", [])
    symbols = index.get("symbols", [])
    relationships = index.get("relationships", [])
    role_counts = Counter(str(item.get("role", "unknown")) for item in files)
    language_counts = Counter(str(item.get("language", "unknown")) for item in files)
    relation_counts = Counter(str(item.get("type", "unknown")) for item in relationships)
    supported = [item for item in files if item.get("parser", {}).get("supported")]
    generated = [item for item in files if item.get("authority") in {"generated", "downloaded"}]
    tracked_files = [item for item in files if item.get("tracked_status") == "tracked"]
    working_tree_files = [item for item in files if item.get("tracked_status") == "untracked"]
    tracked_indexed = sum(1 for item in tracked_files if item.get("index_status") == "indexed")
    return {
        "schema": "treatcode.repository-index-coverage.v1",
        "index_schema": index.get("schema"),
        "commit": index.get("repository", {}).get("commit"),
        "tracked_files": len(tracked_files),
        "indexed_files": sum(1 for item in files if item.get("index_status") == "indexed"),
        "working_tree_files": len(working_tree_files),
        "excluded_files": len(index.get("excluded_files", [])),
        "file_coverage": tracked_indexed / len(tracked_files) if tracked_files else 0.0,
        "parser_supported_files": len(supported),
        "files_with_symbols": len({str(item.get("path")) for item in symbols}),
        "symbols": len(symbols),
        "relationships": len(relationships),
        "relationship_types": dict(sorted(relation_counts.items())),
        "roles": dict(sorted(role_counts.items())),
        "languages": dict(sorted(language_counts.items())),
        "generated_or_downloaded": len(generated),
        "generated_or_downloaded_non_authoritative": all(not bool(item.get("source_authority")) for item in generated),
        "source_reference_count": sum(1 for item in relationships if isinstance(item.get("source"), dict)) + len(symbols) + len(index.get("entities", [])),
        "coverage_dimensions": [
            "files",
            "symbols",
            "source_spans",
            "imports_includes",
            "direct_calls",
            "manifest_edges",
            "test_edges",
            "benchmark_edges",
            "documentation_edges",
            "decision_edges",
        ],
    }


def _freshness(index: dict[str, Any], repo_root: Path, *, exclude_paths: set[str] | None = None) -> dict[str, Any]:
    errors: list[str] = []
    try:
        current_commit = _git_commit(repo_root)
        tracked = _git_paths(repo_root)
        working_tree = _git_untracked_paths(repo_root, exclude_paths=exclude_paths)
        dirty_tracked = sorted(set(_git_dirty_tracked_paths(repo_root)) & set(tracked))
    except RuntimeError as exc:
        current_commit = "unknown"
        tracked = []
        working_tree = []
        dirty_tracked = []
        errors.append(str(exc))
    inventory = sorted(set(tracked) | set(working_tree))
    indexed_files = {str(item.get("path")): item for item in index.get("files", []) if isinstance(item, dict)}
    missing = sorted(set(inventory) - set(indexed_files))
    unexpected = sorted(set(indexed_files) - set(inventory))
    changed: list[str] = []
    working_tree_changed: list[str] = []
    for path in sorted(set(tracked) & set(indexed_files)):
        candidate = _path_within(repo_root, path)
        if not candidate or not candidate.exists():
            if indexed_files[path].get("authority") in {"generated", "downloaded"}:
                continue
            changed.append(path)
            continue
        digest = _sha256_bytes(candidate.read_bytes())
        if digest != indexed_files[path].get("content_sha256"):
            if indexed_files[path].get("tracked_status") == "untracked":
                working_tree_changed.append(path)
            else:
                changed.append(path)
    commit_match = current_commit != "unknown" and current_commit == index.get("repository", {}).get("commit")
    fresh = not errors and commit_match and not missing and not unexpected and not changed and not dirty_tracked
    return {
        "schema": "treatcode.repository-index-freshness.v1",
        "index_commit": index.get("repository", {}).get("commit"),
        "current_commit": current_commit,
        "commit_match": commit_match,
        "tracked_file_count": len(tracked),
        "working_tree_file_count": len(working_tree),
        "inventory_file_count": len(inventory),
        "indexed_file_count": len(indexed_files),
        "missing_files": missing,
        "unexpected_files": unexpected,
        "changed_files": changed,
        "dirty_tracked_files": dirty_tracked,
        "working_tree_changed_files": working_tree_changed,
        "errors": errors,
        "fresh": fresh,
    }


def _index_path(output_dir: Path | str | None, repo_root: Path) -> Path:
    if output_dir is None:
        return repo_root / "build" / "treatcode-index"
    path = Path(output_dir)
    if path.suffix.lower() == ".json":
        return path.parent
    return path


def load_index(path: Path | str | None = None, *, repo_root: Path = DEFAULT_REPO_ROOT) -> tuple[dict[str, Any] | None, str | None, Path]:
    directory = _index_path(path, repo_root)
    file_path = directory if directory.name == INDEX_FILENAME else directory / INDEX_FILENAME
    try:
        data = json.loads(file_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None, f"index is missing: {_posix_path(file_path)}", file_path
    except json.JSONDecodeError as exc:
        return None, f"index JSON is invalid at line {exc.lineno}: {exc.msg}", file_path
    if not isinstance(data, dict):
        return None, "index must be a JSON object", file_path
    return data, None, file_path


def build_index(
    *,
    repo_root: Path = DEFAULT_REPO_ROOT,
    output_dir: Path | str | None = None,
    clean: bool = False,
    commit: str | None = None,
) -> dict[str, Any]:
    repo_root = Path(repo_root).resolve()
    output = _index_path(output_dir, repo_root).resolve()
    output.mkdir(parents=True, exist_ok=True)
    previous: dict[str, Any] | None = None
    if not clean:
        previous, _, _ = load_index(output, repo_root=repo_root)
    snapshot_commit = (commit or _git_commit(repo_root)).lower()
    repository = _repository_name(repo_root)
    tracked_paths = _git_paths(repo_root)
    output_excludes: set[str] = set()
    if output.is_relative_to(repo_root):
        output_excludes.add(_posix_path(output.relative_to(repo_root)))
    untracked_paths = _git_untracked_paths(repo_root, exclude_paths=output_excludes)
    tracked_set = set(tracked_paths) | set(untracked_paths)
    inventory_paths = sorted(tracked_set)
    tracked_authority = set(tracked_paths)
    previous_files = {str(item.get("path")): item for item in (previous or {}).get("files", []) if isinstance(item, dict)}
    files: list[dict[str, Any]] = []
    symbols: list[dict[str, Any]] = []
    parsed: dict[str, dict[str, Any]] = {}
    entities: list[dict[str, Any]] = []
    reused_files = 0
    parsed_files = 0
    metadata_only_files = 0

    for relative in inventory_paths:
        candidate = _path_within(repo_root, relative)
        worktree_present = bool(candidate and candidate.is_file())
        if not worktree_present:
            raw = _git_blob(repo_root, snapshot_commit, relative) or b""
            text = None
            binary = True
        else:
            raw, text, binary = _read_file(candidate)
        language = _language_for(relative)
        role, authority, source_authority, authority_reason = _role_for(relative, language)
        is_tracked = relative in tracked_authority
        if not is_tracked and authority not in {"generated", "downloaded"}:
            authority = "working_tree"
            source_authority = False
            authority_reason = "untracked working-tree input is indexed for development context but is not commit authority"
        digest = _sha256_bytes(raw)
        previous_record = previous_files.get(relative)
        reused = bool(
            previous_record
            and previous_record.get("content_sha256") == digest
            and previous_record.get("language") == language
            and previous_record.get("role") == role
            and previous_record.get("authority") == authority
            and previous_record.get("index_status") == "indexed"
            and previous_record.get("source", {}).get("commit") == snapshot_commit
        )
        if reused:
            reused_files += 1
        parser_supported = bool(
            text is not None
            and language in PARSER_LANGUAGES
            and not binary
            and authority not in {"generated", "downloaded"}
        )
        if parser_supported:
            parsed_files += 1
        else:
            metadata_only_files += 1
        line_count = len(text.splitlines()) if text is not None and text.splitlines() else 0
        record = {
            "id": _file_id(relative),
            "path": relative,
            "language": language,
            "role": role,
            "authority": authority,
            "source_authority": source_authority,
            "authority_reason": authority_reason,
            "tracked_status": "tracked" if is_tracked else "untracked",
            "index_status": "indexed",
            "content_kind": "binary" if binary else "text",
            "snapshot_state": "present" if worktree_present else "missing_from_worktree_using_commit_blob",
            "size_bytes": len(raw),
            "content_sha256": digest,
            "line_count": line_count,
            "parser": {
                "supported": parser_supported,
                "name": "regex-source-span-v1" if parser_supported else "metadata-only-v1",
                "span_support": parser_supported,
            },
            "source": _source_ref(
                repository,
                snapshot_commit,
                relative,
                None,
                role=role,
                status="resolved" if is_tracked else "untracked",
            ),
        }
        files.append(record)
        if parser_supported:
            lines = text.splitlines() if text is not None else []
            file_symbols = _declaration_symbols(relative, language, lines)
            symbols.extend(
                {
                    "id": item["id"],
                    "name": item["name"],
                    "qualified_name": item["qualified_name"],
                    "kind": item["kind"],
                    "path": relative,
                    "language": language,
                    "source_span": item["source_span"],
                    "source": _source_ref(repository, snapshot_commit, relative, item["source_span"], role="symbol"),
                    "signature": item["signature"],
                    "parser": "regex-source-span-v1",
                }
                for item in file_symbols
            )
            parsed[relative] = {
                "repo_root": str(repo_root),
                "text": text or "",
                "symbols": file_symbols,
                "imports": _import_mentions(relative, language, lines),
                "calls": _call_mentions(relative, language, lines, file_symbols),
            }
        elif text is not None and language in {"json", "markdown"}:
            parsed[relative] = {
                "repo_root": str(repo_root),
                "text": text,
                "symbols": [],
                "imports": [],
                "calls": [],
            }
        if text is not None and language == "json":
            entities.extend(_json_entities(relative, text, repository, snapshot_commit))

    relationships = _build_relationships(
        files=files,
        symbols=symbols,
        entities=entities,
        parsed=parsed,
        tracked=tracked_set,
        repository=repository,
        commit=snapshot_commit,
    )
    index: dict[str, Any] = {
        "schema": INDEX_SCHEMA,
        "version": INDEX_VERSION,
        "indexer": {
            "name": "treatcode-repository-ingestion",
            "version": "1.0.0",
            "parsers": ["regex-source-span-v1", "metadata-only-v1"],
        },
        "repository": {
            "name": repository,
            "commit": snapshot_commit,
            "inventory": "git ls-files",
            "snapshot": "working-tree",
        },
        "authority": {
            "source": "git-tracked repository contents",
            "untracked_policy": "selected untracked working-tree inputs are indexed for context only and are never commit authority",
            "generated_policy": "generated and downloaded files remain indexed metadata/evidence and source_authority=false",
            "exclusion_rules": [
                {
                    "id": "untracked",
                    "rule": "git ls-files only",
                    "effect": "not_indexed",
                    "reason": "untracked build/download output is not repository source authority",
                },
                {
                    "id": "untracked-cache-and-generated",
                    "rule": "ignored, cache, generated, and downloaded untracked paths",
                    "effect": "not_indexed",
                    "reason": "only selected working-tree inputs participate in context packages",
                },
                {
                    "id": "unsupported-binary",
                    "rule": "tracked binary or undecodable files",
                    "effect": "metadata_only",
                    "reason": "file inventory and content hash are retained; source parsing is not claimed",
                },
            ],
        },
        "files": files,
        "symbols": symbols,
        "entities": entities,
        "relationships": relationships,
        "excluded_files": [],
    }
    index = _stable_sort_index(index)
    index["coverage"] = _coverage(index)
    index_path = output / INDEX_FILENAME
    index_path.write_text(canonical_json(index), encoding="utf-8")
    report = {
        "schema": "treatcode.repository-index-build-result.v1",
        "ok": True,
        "index": _posix_path(index_path.relative_to(repo_root)) if index_path.is_relative_to(repo_root) else str(index_path),
        "index_schema": INDEX_SCHEMA,
        "commit": snapshot_commit,
        "mode": "clean" if clean else "incremental",
        "tracked_files": len(tracked_paths),
        "working_tree_files": len(untracked_paths),
        "indexed_files": len(files),
        "parsed_files": parsed_files,
        "metadata_only_files": metadata_only_files,
        "symbols": len(symbols),
        "relationships": len(relationships),
        "entities": len(entities),
        "reused_files": reused_files,
        "index_sha256": _sha256_bytes(index_path.read_bytes()),
    }
    (output / "build-result.v1.json").write_text(canonical_json(report), encoding="utf-8")
    return report


def _validate_source_ref(
    reference: Any,
    *,
    repo_root: Path,
    commit: str,
    tracked: set[str],
    line_counts: dict[str, int],
    mutable_paths: set[str] | None,
    location: str,
    errors: list[dict[str, Any]],
) -> None:
    if not isinstance(reference, dict):
        errors.append({"code": "source_reference_type", "location": location, "message": "source reference must be an object"})
        return
    for field in ("repository", "commit", "path", "span"):
        if field not in reference:
            errors.append({"code": "source_reference_field", "location": location, "message": f"source reference is missing {field}"})
    if not isinstance(reference.get("repository"), str) or not str(reference.get("repository")).strip():
        errors.append({"code": "source_reference_repository", "location": location, "message": "repository is required"})
    ref_commit = reference.get("commit")
    if not isinstance(ref_commit, str) or not re.fullmatch(r"[0-9a-fA-F]{7,64}", ref_commit):
        errors.append({"code": "source_reference_commit", "location": location, "message": "commit must be hexadecimal"})
    path = reference.get("path")
    if not isinstance(path, str) or not path.strip():
        errors.append({"code": "source_reference_path", "location": location, "message": "path is required"})
        return
    normal = _normalise_relative(path)
    if reference.get("status", "resolved") == "resolved" and normal not in tracked:
        errors.append({"code": "source_reference_target", "location": location, "message": f"resolved source path is not in the repository: {normal}"})
    span = reference.get("span")
    if span is None:
        return
    if not isinstance(span, dict):
        errors.append({"code": "source_span_type", "location": location, "message": "span must be null or an object"})
        return
    fields = ("start_line", "start_column", "end_line", "end_column")
    if any(not isinstance(span.get(field), int) or span.get(field) < 1 for field in fields):
        errors.append({"code": "source_span_shape", "location": location, "message": "span coordinates must be positive integers"})
        return
    span_path = _normalise_relative(str(reference.get("span_path", normal)))
    if normal in (mutable_paths or set()) or span_path in (mutable_paths or set()):
        return
    count = line_counts.get(span_path)
    if count is not None and (span["start_line"] > count or span["end_line"] > count):
        errors.append({"code": "source_span_bounds", "location": location, "message": f"span is outside {normal} ({count} lines)"})


def _collect_nested_source_refs(value: Any, location: str = "$") -> Iterable[tuple[str, dict[str, Any]]]:
    if isinstance(value, dict):
        if {"repository", "commit", "path", "span"}.issubset(value):
            yield location, value
        for key, child in value.items():
            yield from _collect_nested_source_refs(child, f"{location}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from _collect_nested_source_refs(child, f"{location}[{index}]")


def verify_index(
    *,
    repo_root: Path = DEFAULT_REPO_ROOT,
    index_path: Path | str | None = None,
    evidence_dir: Path | str | None = None,
) -> dict[str, Any]:
    repo_root = Path(repo_root).resolve()
    index, load_error, resolved_path = load_index(index_path, repo_root=repo_root)
    if load_error or index is None:
        return {
            "schema": "treatcode.repository-index-verification.v1",
            "ok": False,
            "index": _posix_path(resolved_path),
            "errors": [{"code": "index_load_failed", "message": load_error or "index is missing"}],
        }
    errors: list[dict[str, Any]] = []
    if index.get("schema") != INDEX_SCHEMA:
        errors.append({"code": "index_schema", "message": f"expected {INDEX_SCHEMA}"})
    if index.get("version") != INDEX_VERSION:
        errors.append({"code": "index_version", "message": "unsupported index version"})
    index_excludes: set[str] = set()
    if resolved_path.parent.is_relative_to(repo_root):
        index_excludes.add(_posix_path(resolved_path.parent.relative_to(repo_root)))
    try:
        git_tracked = set(_git_paths(repo_root))
        working_tree = set(_git_untracked_paths(repo_root, exclude_paths=index_excludes))
        tracked = git_tracked | working_tree
        current_commit = _git_commit(repo_root)
    except RuntimeError as exc:
        tracked = set()
        git_tracked = set()
        working_tree = set()
        current_commit = "unknown"
        errors.append({"code": "git_inventory", "message": str(exc)})
    files = index.get("files") if isinstance(index.get("files"), list) else []
    indexed_paths = {str(item.get("path")): item for item in files if isinstance(item, dict)}
    if set(indexed_paths) != tracked:
        errors.append(
            {
                "code": "file_coverage",
                "message": "indexed file inventory does not equal tracked plus selected working-tree inventory",
                "missing": sorted(tracked - set(indexed_paths)),
                "unexpected": sorted(set(indexed_paths) - tracked),
            }
        )
    index_commit = index.get("repository", {}).get("commit") if isinstance(index.get("repository"), dict) else None
    if current_commit != "unknown" and index_commit != current_commit:
        errors.append({"code": "commit_stale", "message": f"index commit {index_commit!r} != current commit {current_commit!r}"})
    line_counts: dict[str, int] = {}
    for path, item in indexed_paths.items():
        candidate = _path_within(repo_root, path)
        if candidate and candidate.is_file():
            raw, text, _ = _read_file(candidate)
            line_counts[path] = len(text.splitlines()) if text is not None and text.splitlines() else 0
            if _sha256_bytes(raw) != item.get("content_sha256"):
                if item.get("tracked_status") != "untracked":
                    errors.append({"code": "file_hash_stale", "message": f"content hash changed for {path}", "path": path})
        elif path in tracked and item.get("tracked_status") != "untracked" and item.get("authority") not in {"generated", "downloaded"}:
            errors.append({"code": "file_missing", "message": f"tracked file is not readable: {path}", "path": path})
        if item.get("authority") in {"generated", "downloaded"} and item.get("source_authority") is not False:
            errors.append({"code": "generated_source_authority", "message": f"generated/downloaded file promoted to source authority: {path}"})
        if item.get("index_status") not in {"indexed"}:
            errors.append({"code": "file_index_status", "message": f"file is not indexed: {path}"})
        _validate_source_ref(
            item.get("source"),
            repo_root=repo_root,
            commit=str(index_commit),
            tracked=tracked,
            line_counts=line_counts,
            mutable_paths=working_tree,
            location=f"$.files[{path}].source",
            errors=errors,
        )
    symbols = index.get("symbols") if isinstance(index.get("symbols"), list) else []
    entities = index.get("entities") if isinstance(index.get("entities"), list) else []
    relationships = index.get("relationships") if isinstance(index.get("relationships"), list) else []
    for collection_name, collection in (("symbols", symbols), ("entities", entities), ("relationships", relationships)):
        seen_ids: set[str] = set()
        for position, item in enumerate(collection):
            if not isinstance(item, dict):
                errors.append({"code": "record_type", "message": f"{collection_name}[{position}] must be an object"})
                continue
            item_id = item.get("id")
            if not isinstance(item_id, str) or not item_id:
                errors.append({"code": "record_id", "message": f"{collection_name}[{position}] has no id"})
            elif item_id in seen_ids:
                errors.append({"code": "record_duplicate_id", "message": f"duplicate {collection_name} id {item_id}"})
            seen_ids.add(str(item_id))
    for position, symbol in enumerate(symbols):
        if isinstance(symbol, dict):
            _validate_source_ref(symbol.get("source"), repo_root=repo_root, commit=str(index_commit), tracked=tracked, line_counts=line_counts, mutable_paths=working_tree, location=f"$.symbols[{position}].source", errors=errors)
    for position, entity in enumerate(entities):
        if isinstance(entity, dict):
            _validate_source_ref(entity.get("source"), repo_root=repo_root, commit=str(index_commit), tracked=tracked, line_counts=line_counts, mutable_paths=working_tree, location=f"$.entities[{position}].source", errors=errors)
            for location, ref in _collect_nested_source_refs(entity.get("data"), f"$.entities[{position}].data"):
                _validate_source_ref(ref, repo_root=repo_root, commit=str(index_commit), tracked=tracked, line_counts=line_counts, mutable_paths=working_tree, location=location, errors=errors)
    for position, relationship in enumerate(relationships):
        if not isinstance(relationship, dict):
            continue
        _validate_source_ref(relationship.get("source"), repo_root=repo_root, commit=str(index_commit), tracked=tracked, line_counts=line_counts, mutable_paths=working_tree, location=f"$.relationships[{position}].source", errors=errors)
        if relationship.get("target_source") is not None:
            _validate_source_ref(relationship.get("target_source"), repo_root=repo_root, commit=str(index_commit), tracked=tracked, line_counts=line_counts, mutable_paths=working_tree, location=f"$.relationships[{position}].target_source", errors=errors)
        resolution = relationship.get("resolution")
        target = str(relationship.get("to", ""))
        if resolution == "resolved" and not (target.startswith("file:") or target.startswith("symbol:") or target.startswith("record:")):
            errors.append({"code": "relationship_target", "message": f"resolved relationship has invalid target {target}"})
    coverage = _coverage(index)
    freshness = _freshness(index, repo_root, exclude_paths=index_excludes)
    if not freshness.get("fresh"):
        errors.append({"code": "index_not_fresh", "message": "index does not describe the current Git snapshot", "details": freshness})
    report = {
        "schema": "treatcode.repository-index-verification.v1",
        "ok": not errors,
        "index": _posix_path(resolved_path),
        "commit": index_commit,
        "errors": errors,
        "coverage": coverage,
        "freshness": freshness,
        "index_sha256": _sha256_bytes(resolved_path.read_bytes()),
    }
    evidence = Path(evidence_dir) if evidence_dir else repo_root / "build" / "treatcode-plan-evidence" / "P03"
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / "index-coverage.json").write_text(canonical_json(coverage), encoding="utf-8")
    (evidence / "index-freshness.json").write_text(canonical_json(freshness), encoding="utf-8")
    (resolved_path.parent / COVERAGE_FILENAME).write_text(canonical_json(coverage), encoding="utf-8")
    (resolved_path.parent / FRESHNESS_FILENAME).write_text(canonical_json(freshness), encoding="utf-8")
    return report


def _scope_match(scope: str, item: dict[str, Any]) -> bool:
    value = scope.strip()
    if not value:
        return False
    candidates = {str(item.get("id", "")), str(item.get("path", "")), str(item.get("name", "")), str(item.get("qualified_name", ""))}
    if value.startswith("file:") or value.startswith("symbol:") or value.startswith("record:"):
        return value in candidates
    return value in candidates or value.replace("\\", "/") in candidates


def generate_context_package(
    index: dict[str, Any],
    scopes: Sequence[str],
    *,
    output_path: Path | str | None = None,
    repo_root: Path = DEFAULT_REPO_ROOT,
) -> dict[str, Any]:
    repo_root = Path(repo_root).resolve()
    requested = [str(scope).strip() for scope in scopes if str(scope).strip()]
    files = [item for item in index.get("files", []) if isinstance(item, dict)]
    symbols = [item for item in index.get("symbols", []) if isinstance(item, dict)]
    entities = [item for item in index.get("entities", []) if isinstance(item, dict)]
    relationships = [item for item in index.get("relationships", []) if isinstance(item, dict)]
    file_by_id = {str(item.get("id")): item for item in files}
    symbol_by_id = {str(item.get("id")): item for item in symbols}
    entity_by_id = {str(item.get("id")): item for item in entities}
    requested_files: set[str] = set()
    requested_symbols: set[str] = set()
    requested_entities: set[str] = set()
    for scope in requested:
        for item in files:
            if _scope_match(scope, item):
                requested_files.add(str(item["id"]))
        for item in symbols:
            if _scope_match(scope, item):
                requested_symbols.add(str(item["id"]))
        for item in entities:
            if _scope_match(scope, item) or scope == str(item.get("record_id", "")):
                requested_entities.add(str(item["id"]))
    for symbol_id in list(requested_symbols):
        symbol = symbol_by_id.get(symbol_id)
        if symbol:
            requested_files.add(_file_id(str(symbol.get("path"))))
    errors: list[dict[str, Any]] = []
    if not requested_files and not requested_symbols and not requested_entities:
        errors.append({"code": "scope_not_found", "message": "no indexed file, symbol, or manifest record matched the requested scope", "scope": requested})

    selected_file_ids = set(requested_files)
    selected_symbol_ids = set(requested_symbols)
    selected_entity_ids = set(requested_entities)
    selection_reasons: dict[str, list[str]] = defaultdict(list)
    for item_id in selected_file_ids:
        selection_reasons[item_id].append("requested_scope")
    for item_id in selected_symbol_ids:
        selection_reasons[item_id].append("requested_symbol")
    for item_id in selected_entity_ids:
        selection_reasons[item_id].append("requested_record")
    for symbol in symbols:
        if _file_id(str(symbol.get("path"))) in selected_file_ids:
            symbol_id = str(symbol.get("id"))
            selected_symbol_ids.add(symbol_id)
            selection_reasons[symbol_id].append("requested_file_symbol")

    direct_relation_types = {"imports", "includes", "calls", "depends_on"}
    for relationship in relationships:
        source_id = str(relationship.get("from", ""))
        if source_id not in selected_file_ids and source_id not in selected_symbol_ids and source_id not in selected_entity_ids:
            continue
        target = str(relationship.get("to", ""))
        if target in file_by_id:
            if relationship.get("type") in direct_relation_types:
                selected_file_ids.add(target)
                selection_reasons[target].append("direct_dependency")
        elif target in symbol_by_id and relationship.get("type") in direct_relation_types:
            selected_symbol_ids.add(target)
            selected_file_ids.add(_file_id(str(symbol_by_id[target].get("path"))))
            selection_reasons[target].append("direct_dependency")
            selection_reasons[_file_id(str(symbol_by_id[target].get("path")))].append("direct_dependency")
        elif target in entity_by_id and relationship.get("type") in {"depends_on", "contracts", "capabilities", "gaps", "releases"}:
            selected_entity_ids.add(target)
            selection_reasons[target].append("required_contract_or_evidence")

    relevant_file_ids = set(selected_file_ids)
    relevant_entity_ids = set(selected_entity_ids)
    semantic_relation_types = {"contracts", "capabilities", "gaps", "decisions", "releases", "depends_on"}
    # Add the authority records directly attached to the selected files, and
    # the source files for explicitly selected records. This is intentionally
    # a single bounded hop; traversing manifest/semantic edges to a fixed point
    # would turn a focused package into the whole project graph.
    for relationship in relationships:
        target = str(relationship.get("to", ""))
        source = str(relationship.get("from", ""))
        relation_type = str(relationship.get("type", ""))
        if relation_type != "manifest":
            continue
        if target in relevant_file_ids and source in entity_by_id:
            relevant_entity_ids.add(source)
            selection_reasons[source].append("manifest_record")
        if source in relevant_entity_ids and target in file_by_id:
            relevant_file_ids.add(target)
            selection_reasons[target].append("record_source")

    # Pull only the direct semantic contracts/evidence of the records selected
    # above. The source set is frozen so newly selected records do not recurse
    # into unrelated parts of the authority graph.
    semantic_seed_entity_ids = set(relevant_entity_ids)
    for relationship in relationships:
        source = str(relationship.get("from", ""))
        target = str(relationship.get("to", ""))
        if (
            source in semantic_seed_entity_ids
            and relationship.get("type") in semantic_relation_types
            and target in entity_by_id
        ):
            relevant_entity_ids.add(target)
            selection_reasons[target].append("required_contract_or_evidence")

    # Include source files for the newly selected records, then the directly
    # linked tests, benchmarks, and documentation. These are also one-hop
    # selections and remain bounded by the requested scope.
    for relationship in relationships:
        target = str(relationship.get("to", ""))
        source = str(relationship.get("from", ""))
        relation_type = str(relationship.get("type", ""))
        if relation_type == "manifest" and source in relevant_entity_ids and target in file_by_id:
            relevant_file_ids.add(target)
            selection_reasons[target].append("record_source")
        if target in relevant_file_ids and relation_type in {"tests", "benchmarks", "documents"} and source in file_by_id:
            relevant_file_ids.add(source)
            selection_reasons[source].append("required_" + relation_type.rstrip("s"))
    selected_file_ids = relevant_file_ids
    selected_entity_ids = relevant_entity_ids

    selected_files = []
    for item in files:
        if str(item.get("id")) in selected_file_ids:
            selected = copy.deepcopy(item)
            selected["selection_reasons"] = sorted(set(selection_reasons.get(str(item["id"]), [])))
            selected_files.append(selected)
    selected_symbols = [copy.deepcopy(item) for item in symbols if str(item.get("id")) in selected_symbol_ids or _file_id(str(item.get("path"))) in selected_file_ids]
    selected_entities = []
    for item in entities:
        if str(item.get("id")) in selected_entity_ids:
            selected = copy.deepcopy(item)
            selected["selection_reasons"] = sorted(set(selection_reasons.get(str(item["id"]), [])))
            selected_entities.append(selected)
    included_ids = {str(item["id"]) for item in selected_files} | {str(item["id"]) for item in selected_symbols} | {str(item["id"]) for item in selected_entities}
    selected_relationships = []
    for relationship in relationships:
        source_id = str(relationship.get("from", ""))
        target_id = str(relationship.get("to", ""))
        if source_id in included_ids and (target_id in included_ids or str(relationship.get("resolution")) != "resolved"):
            selected_relationships.append(copy.deepcopy(relationship))

    package: dict[str, Any] = {
        "schema": CONTEXT_SCHEMA,
        "version": 1,
        "repository": copy.deepcopy(index.get("repository", {})),
        "scope": requested,
        "selection": {
            "requested_files": sorted(requested_files),
            "requested_symbols": sorted(requested_symbols),
            "requested_records": sorted(requested_entities),
            "bounded": True,
            "direct_dependency_depth": 1,
        },
        "files": sorted(selected_files, key=lambda item: item.get("path", "")),
        "symbols": sorted(selected_symbols, key=lambda item: item.get("id", "")),
        "entities": sorted(selected_entities, key=lambda item: item.get("id", "")),
        "relationships": sorted(selected_relationships, key=lambda item: item.get("id", "")),
        "required_contracts": sorted([item for item in selected_entities if item.get("kind") == "contract"], key=lambda item: item.get("id", "")),
        "tests": sorted([item for item in selected_files if item.get("role") == "test"], key=lambda item: item.get("path", "")),
        "gaps": sorted([item for item in selected_entities if item.get("kind") == "gap"], key=lambda item: item.get("id", "")),
        "baselines": sorted([item for item in selected_files if item.get("role") == "benchmark"], key=lambda item: item.get("path", "")),
        "documentation": sorted([item for item in selected_files if item.get("role") == "documentation"], key=lambda item: item.get("path", "")),
        "errors": errors,
    }
    stable_package = copy.deepcopy(package)
    package["package_sha256"] = _sha256_text(canonical_json(stable_package))
    if output_path is not None:
        destination = Path(output_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(canonical_json(package), encoding="utf-8")
        package["output"] = _posix_path(destination.relative_to(repo_root)) if destination.is_relative_to(repo_root) else str(destination)
    return package


def compare_clean_incremental(
    *,
    repo_root: Path = DEFAULT_REPO_ROOT,
    evidence_dir: Path | str | None = None,
) -> dict[str, Any]:
    repo_root = Path(repo_root).resolve()
    build_root = repo_root / "build"
    build_root.mkdir(parents=True, exist_ok=True)
    temp_root = Path(tempfile.mkdtemp(prefix="treatcode-index-compare-", dir=str(build_root)))
    try:
        clean_dir = temp_root / "clean"
        incremental_dir = temp_root / "incremental"
        clean_result = build_index(repo_root=repo_root, output_dir=clean_dir, clean=True)
        build_index(repo_root=repo_root, output_dir=incremental_dir, clean=True)
        incremental_result = build_index(repo_root=repo_root, output_dir=incremental_dir, clean=False)
        clean_index = (clean_dir / INDEX_FILENAME).read_bytes()
        incremental_index = (incremental_dir / INDEX_FILENAME).read_bytes()
        clean_hash = _sha256_bytes(clean_index)
        incremental_hash = _sha256_bytes(incremental_index)
        clean_data = json.loads(clean_index.decode("utf-8"))
        package_path = (Path(evidence_dir) if evidence_dir else repo_root / "build" / "treatcode-plan-evidence" / "P03") / "context-package.v1.json"
        package = generate_context_package(clean_data, ["kernel.trit"], output_path=package_path, repo_root=repo_root)
        report = {
            "schema": "treatcode.repository-index-determinism.v1",
            "ok": clean_hash == incremental_hash and bool(package.get("package_sha256")) and not package.get("errors"),
            "commit": clean_result.get("commit"),
            "clean": {
                "index_sha256": clean_hash,
                "tracked_files": clean_result.get("tracked_files"),
                "symbols": clean_result.get("symbols"),
                "relationships": clean_result.get("relationships"),
            },
            "incremental": {
                "index_sha256": incremental_hash,
                "tracked_files": incremental_result.get("tracked_files"),
                "symbols": incremental_result.get("symbols"),
                "relationships": incremental_result.get("relationships"),
                "reused_files": incremental_result.get("reused_files"),
            },
            "equivalent": clean_hash == incremental_hash,
            "context_package": {
                "scope": ["kernel.trit"],
                "path": package.get("output"),
                "sha256": package.get("package_sha256"),
                "files": len(package.get("files", [])),
                "relationships": len(package.get("relationships", [])),
            },
        }
        destination = Path(evidence_dir) if evidence_dir else repo_root / "build" / "treatcode-plan-evidence" / "P03"
        destination.mkdir(parents=True, exist_ok=True)
        (destination / DETERMINISM_FILENAME).write_text(canonical_json(report), encoding="utf-8")
        return report
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)


def print_report(report: dict[str, Any], title: str, *, json_output: bool = False) -> int:
    if json_output:
        print(canonical_json(report), end="")
    else:
        print(title)
        print(f"[{'ok' if report.get('ok') else 'fail'}] {report.get('schema', 'report')}")
        for key in ("index", "commit", "tracked_files", "symbols", "relationships", "equivalent", "fresh"):
            if key in report:
                print(f"{key}: {report[key]}")
        for error in report.get("errors", [])[:20]:
            print(f"- {error.get('code', 'error')}: {error.get('message', error)}")
    return 0 if report.get("ok") else 1


__all__ = [
    "CONTEXT_SCHEMA",
    "INDEX_FILENAME",
    "INDEX_SCHEMA",
    "build_index",
    "compare_clean_incremental",
    "generate_context_package",
    "load_index",
    "verify_index",
]
