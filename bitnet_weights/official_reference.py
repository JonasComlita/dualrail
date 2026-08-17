#!/usr/bin/env python3
"""Capture repeatable token-ID evidence from the pinned official BitNet runner.

This wrapper intentionally has no synthetic or local-model fallback.  It only
records evidence when an operator supplies a real runner executable, its
immutable revision, the official GGUF, and two identical deterministic runs.
The runner must emit one machine-readable line::

    token_ids: 123, 456, 789

Extra ``--runner-arg`` values are available for a pinned BitNet/llama.cpp
checkout whose token logging flags differ from the defaults.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence


REFERENCE_SCHEMA = "trit.bitnet_reference.v1"
TOKEN_LINE = re.compile(r"(?:^|\n)\s*token_ids\s*:\s*\[?([^\]\n]+)\]?", re.IGNORECASE)
INPUT_TOKEN_LINE = re.compile(
    r"(?:^|\n)\s*input_token_ids\s*:\s*\[?([^\]\n]+)\]?", re.IGNORECASE
)


class ReferenceError(ValueError):
    """The official runner/evidence contract was not satisfied."""


def sha256_file(path: Path, chunk_bytes: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_bytes):
            digest.update(chunk)
    return digest.hexdigest()


def _runner_path(value: str) -> str:
    candidate = Path(value)
    if candidate.is_file():
        return str(candidate.resolve())
    resolved = shutil.which(value)
    if resolved:
        return str(Path(resolved).resolve())
    raise ReferenceError(f"official runner is unavailable: {value}")


def _runner_command(path: str) -> list[str]:
    """Return an executable command for native binaries and Python adapters."""
    if path.lower().endswith(".py"):
        return [sys.executable, path]
    return [path]


def _parse_token_ids(output: str, expected_length: int) -> list[int]:
    matches = list(TOKEN_LINE.finditer(output))
    if not matches:
        raise ReferenceError(
            "official runner output has no machine-readable 'token_ids:' line; "
            "enable token logging in the pinned runner"
        )
    raw = matches[-1].group(1)
    try:
        tokens = [int(piece) for piece in re.split(r"[,\s]+", raw.strip()) if piece]
    except ValueError as exc:
        raise ReferenceError("official runner token_ids line is not an integer list") from exc
    if len(tokens) != expected_length:
        raise ReferenceError(
            f"official runner returned {len(tokens)} token IDs; expected fixed length {expected_length}"
        )
    if any(token < 0 for token in tokens):
        raise ReferenceError("official runner returned a negative token ID")
    return tokens


def _parse_optional_input_token_ids(output: str) -> list[int] | None:
    matches = list(INPUT_TOKEN_LINE.finditer(output))
    if not matches:
        return None
    raw = matches[-1].group(1)
    try:
        tokens = [int(piece) for piece in re.split(r"[,\s]+", raw.strip()) if piece]
    except ValueError as exc:
        raise ReferenceError("official runner input_token_ids line is not an integer list") from exc
    if any(token < 0 for token in tokens):
        raise ReferenceError("official runner returned a negative input token ID")
    return tokens


def token_hash(token_ids: Sequence[int]) -> str:
    canonical = ",".join(str(int(token)) for token in token_ids).encode("ascii")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def run_reference(
    *,
    runner: str,
    runner_revision: str,
    model: str | os.PathLike[str],
    prompt: str,
    length: int = 32,
    threads: int = 4,
    runs: int = 2,
    expected_model_sha256: str | None = None,
    input_token_ids: Sequence[int] = (),
    runner_args: Sequence[str] = (),
    timeout_seconds: int = 900,
    output: str | os.PathLike[str] | None = None,
) -> dict[str, Any]:
    if not runner_revision.strip():
        raise ReferenceError("--runner-revision is required; an unpinned runner cannot produce evidence")
    if not prompt:
        raise ReferenceError("--prompt is required for deterministic reference execution")
    if not input_token_ids:
        raise ReferenceError(
            "--input-token-ids is required; the Trit executor must receive the "
            "same fixed prompt IDs as the official runner"
        )
    if length <= 0 or threads <= 0:
        raise ReferenceError("length and threads must be positive")
    if runs != 2:
        raise ReferenceError("exactly two reference runs are required")
    model_path = Path(model).resolve()
    if not model_path.is_file():
        raise ReferenceError(f"official GGUF model is unavailable: {model_path}")
    model_sha256 = sha256_file(model_path)
    if expected_model_sha256 and model_sha256 != expected_model_sha256:
        raise ReferenceError(
            f"official GGUF SHA-256 mismatch: expected {expected_model_sha256}, observed {model_sha256}"
        )
    runner_path = _runner_path(runner)
    command = [
        *_runner_command(runner_path),
        "-m", str(model_path),
        "-p", prompt,
        "-n", str(length),
        "-t", str(threads),
        "--temp", "0",
        "--seed", "0",
        *runner_args,
    ]
    observed: list[list[int]] = []
    for run_number in range(1, runs + 1):
        try:
            completed = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_seconds,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise ReferenceError(f"official runner failed on reference run {run_number}: {exc}") from exc
        if completed.returncode != 0:
            raise ReferenceError(
                f"official runner exited {completed.returncode} on reference run {run_number}: "
                f"{completed.stderr[-1000:]}"
            )
        runner_output = completed.stdout + "\n" + completed.stderr
        observed_input = _parse_optional_input_token_ids(runner_output)
        if observed_input is not None and observed_input != [int(token) for token in input_token_ids]:
            raise ReferenceError(
                f"official runner prompt IDs differ from the fixed input IDs: "
                f"{observed_input} != {list(input_token_ids)}"
            )
        observed.append(_parse_token_ids(runner_output, length))

    matching = observed[0] == observed[1]
    evidence: dict[str, Any] = {
        "schema": REFERENCE_SCHEMA,
        "version": 1,
        "status": "pass" if matching else "mismatch",
        "matching_runs": runs if matching else 0,
        "runner": runner_path,
        "runner_revision": runner_revision,
        "model": str(model_path),
        "model_sha256": model_sha256,
        "prompt": prompt,
        "input_token_ids": [int(token) for token in input_token_ids],
        "length": length,
        "threads": threads,
        "greedy": True,
        "runs": observed,
        "token_ids": observed[0],
        "token_hash": token_hash(observed[0]),
    }
    if output is not None:
        _atomic_json(Path(output).resolve(), evidence)
    if not matching:
        raise ReferenceError("official runner produced different token IDs across the two required runs")
    return evidence


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--runner-revision", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--prompt", required=True)
    parser.add_argument("--length", type=int, default=32)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--expected-model-sha256")
    parser.add_argument(
        "--input-token-ids", required=True,
        help="comma-separated fixed prompt token IDs passed to the Trit executor",
    )
    parser.add_argument("--runner-arg", action="append", default=[])
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--output", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        input_ids = tuple(
            int(piece.strip())
            for piece in args.input_token_ids.split(",")
            if piece.strip()
        )
        result = run_reference(
            runner=args.runner,
            runner_revision=args.runner_revision,
            model=args.model,
            prompt=args.prompt,
            length=args.length,
            threads=args.threads,
            runs=args.runs,
            expected_model_sha256=args.expected_model_sha256,
            input_token_ids=input_ids,
            runner_args=args.runner_arg,
            timeout_seconds=args.timeout_seconds,
            output=args.output,
        )
    except (ReferenceError, OSError, ValueError) as exc:
        print(f"BitNet official reference blocked: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
