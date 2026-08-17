#!/usr/bin/env python3
"""Adapt the pinned BitNet/llama.cpp tools to the reference token contract.

The pinned completion executable writes generated pieces, while the pinned
tokenizer executable exposes token IDs.  This adapter runs both binaries from
the same immutable checkout, removes only the completion tool's documented
terminal formatting, and accepts the result only when tokenizing the complete
prompt reproduces the prompt prefix and exactly the requested number of
generated IDs.

The adapter deliberately has no model, tokenizer, or synthetic-output
fallback.  Set ``TRIT_BITNET_OFFICIAL_COMPLETION_RUNNER`` and
``TRIT_BITNET_OFFICIAL_TOKENIZER_RUNNER`` (or pass the corresponding options)
to executables from the pinned BitNet checkout.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence


class AdapterError(ValueError):
    """The pinned runner pair did not satisfy the reference contract."""


def _machine_ids(output: str, label: str) -> list[int]:
    """Parse one exact machine-readable token-ID line."""

    pattern = re.compile(rf"(?m)^\s*{re.escape(label)}\s*:\s*([^\r\n]+)\s*$")
    matches = pattern.findall(output)
    if not matches:
        raise AdapterError(f"official token runner emitted no {label}: line")
    raw = matches[-1].strip().strip("[]")
    try:
        values = [int(piece) for piece in re.split(r"[,\s]+", raw) if piece]
    except ValueError as exc:
        raise AdapterError(f"official token runner emitted an invalid {label}: line") from exc
    if any(value < 0 for value in values):
        raise AdapterError(f"official token runner emitted a negative {label}")
    return values


def _resolve(value: str, label: str) -> str:
    candidate = Path(value)
    if candidate.is_file():
        return str(candidate.resolve())
    resolved = shutil.which(value)
    if resolved:
        return str(Path(resolved).resolve())
    raise AdapterError(f"{label} is unavailable: {value}")


def _token_ids(output: str) -> list[int]:
    values: list[int] = []
    for line in output.splitlines():
        left, separator, _ = line.partition("->")
        if not separator:
            continue
        token = left.strip()
        if token.isdigit():
            values.append(int(token))
    if not values:
        raise AdapterError("pinned tokenizer emitted no token IDs")
    return values


def _run(command: Sequence[str], timeout: int, label: str) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            list(command),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AdapterError(f"{label} failed: {exc}") from exc
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout)[-1000:]
        raise AdapterError(f"{label} exited {completed.returncode}: {detail}")
    return completed


def capture_token_ids(
    *,
    completion_runner: str,
    tokenizer_runner: str,
    model: str | os.PathLike[str],
    prompt: str,
    length: int,
    threads: int,
    seed: int = 0,
    timeout_seconds: int = 900,
    token_runner: str | None = None,
) -> tuple[list[int], list[int]]:
    if length <= 0 or threads <= 0:
        raise AdapterError("length and threads must be positive")
    if seed != 0:
        raise AdapterError("the official reference requires seed 0")
    model_path = Path(model).resolve()
    if not model_path.is_file():
        raise AdapterError(f"model is unavailable: {model_path}")
    if token_runner:
        token_runner_path = _resolve(token_runner, "official token runner")
        token_run = _run(
            (
                token_runner_path,
                "--model", str(model_path),
                "--prompt", prompt,
                "--length", str(length),
                "--threads", str(threads),
            ),
            timeout_seconds,
            "official token runner",
        )
        prompt_tokens = _machine_ids(token_run.stdout + "\n" + token_run.stderr, "input_token_ids")
        generated_tokens = _machine_ids(token_run.stdout + "\n" + token_run.stderr, "token_ids")
        if len(generated_tokens) != length:
            raise AdapterError(
                f"official token runner returned {len(generated_tokens)} generated IDs; "
                f"expected {length}"
            )
        return prompt_tokens, generated_tokens

    completion_path = _resolve(completion_runner, "official completion runner")
    tokenizer_path = _resolve(tokenizer_runner, "official tokenizer runner")

    completion = _run(
        (
            completion_path,
            "-m", str(model_path),
            "-p", prompt,
            "-n", str(length),
            "-t", str(threads),
            "--temp", "0",
            "--seed", "0",
            "-no-cnv",
            "--no-display-prompt",
            "--simple-io",
        ),
        timeout_seconds,
        "official completion runner",
    )
    generated = completion.stdout
    # llama-completion appends exactly two newlines after the generated stream.
    if not generated.endswith("\n\n"):
        raise AdapterError("official completion output is missing its terminal protocol")
    generated = generated[:-2]

    prompt_tokens = _token_ids(
        _run(
            (tokenizer_path, "-m", str(model_path), "-p", prompt, "--show-count"),
            timeout_seconds,
            "official tokenizer prompt pass",
        ).stdout
    )
    combined_tokens = _token_ids(
        _run(
            (
                tokenizer_path,
                "-m", str(model_path),
                "-p", prompt + generated,
                "--show-count",
            ),
            timeout_seconds,
            "official tokenizer completion pass",
        ).stdout
    )
    if combined_tokens[:len(prompt_tokens)] != prompt_tokens:
        raise AdapterError("combined tokenization changed the official prompt token prefix")
    generated_tokens = combined_tokens[len(prompt_tokens):]
    if len(generated_tokens) != length:
        raise AdapterError(
            f"official completion tokenization returned {len(generated_tokens)} generated IDs; "
            f"expected {length}"
        )
    return prompt_tokens, generated_tokens


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-m", "--model", required=True)
    parser.add_argument("-p", "--prompt", required=True)
    parser.add_argument("-n", "--predict", "--n-predict", dest="length", type=int, required=True)
    parser.add_argument("-t", "--threads", type=int, required=True)
    parser.add_argument("--temp", default="0")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--completion-runner",
        default=os.environ.get("TRIT_BITNET_OFFICIAL_COMPLETION_RUNNER", "llama-completion"),
    )
    parser.add_argument(
        "--tokenizer-runner",
        default=os.environ.get("TRIT_BITNET_OFFICIAL_TOKENIZER_RUNNER", "llama-tokenize"),
    )
    parser.add_argument(
        "--token-runner",
        default=os.environ.get("TRIT_BITNET_OFFICIAL_TOKEN_RUNNER"),
        help="optional pinned llama.cpp token-level runner; preferred when set",
    )
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if str(args.temp) != "0":
            raise AdapterError("the official reference requires temperature 0")
        prompt_tokens, generated_tokens = capture_token_ids(
            completion_runner=args.completion_runner,
            tokenizer_runner=args.tokenizer_runner,
            model=args.model,
            prompt=args.prompt,
            length=args.length,
            threads=args.threads,
            seed=args.seed,
            timeout_seconds=args.timeout_seconds,
            token_runner=args.token_runner,
        )
    except (AdapterError, OSError, ValueError) as exc:
        print(f"BitNet official runner adapter blocked: {exc}", file=sys.stderr)
        return 2
    print("input_token_ids: " + ", ".join(str(token) for token in prompt_tokens))
    print("token_ids: " + ", ".join(str(token) for token in generated_tokens))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
