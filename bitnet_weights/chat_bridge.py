"""
chat_bridge.py — Python front-end for run_bitnet.exe

Reads token ids from the C++ process stdout line-by-line.
Displays generated text inline and per-token timing in a subtle status line.

Protocol (stdout from run_bitnet.exe):
  "Generated token: N (cache=K) [matmul=Xms norm=Yms sampling=Zms]"
  "Next predicted token: N"
  "=== Generation Stats ==="
  ... stat lines ...
  "========================"
"""

import subprocess
import sys
import os
import re
import json
import time
from tokenizers import Tokenizer

# ---------------------------------------------------------------------------
# Path configuration
# ---------------------------------------------------------------------------
BASE_DIR       = os.path.dirname(os.path.abspath(__file__))
TOKENIZER_PATH = os.path.join(BASE_DIR, "model", "tokenizer.json")
CONFIG_PATH    = os.path.join(BASE_DIR, "model", "config.json")
WEIGHTS_DIR    = os.path.join(BASE_DIR, "converted")
CONFIG_PATH    = os.path.join(BASE_DIR, "model", "config.json")

# Build output is one level up from bitnet_weights/
BUILD_DIR      = os.path.join(BASE_DIR, "..", "build")
EXE_NAME       = "run_bitnet.exe" if sys.platform == "win32" else "run_bitnet"
RUN_BITNET_EXE = os.path.join(BUILD_DIR, EXE_NAME)

DEFAULT_MAX_NEW_TOKENS = 200
DEFAULT_TEMPERATURE    = 0.7
DEFAULT_TOP_P          = 0.95
DEFAULT_PENALTY        = 1.15

# ---------------------------------------------------------------------------
# ANSI helpers (disabled if not a tty)
# ---------------------------------------------------------------------------
_USE_COLOR = sys.stdout.isatty()

def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text

def dim(t):   return _c("2", t)
def bold(t):  return _c("1", t)
def green(t): return _c("32", t)
def cyan(t):  return _c("36", t)
def red(t):   return _c("31", t)

# ---------------------------------------------------------------------------
# Token / config loading
# ---------------------------------------------------------------------------

def load_token_ids(tokenizer):
    bos_id = tokenizer.token_to_id("<|begin_of_text|>") or 128000
    eos_ids = set()
    for name in ("<|end_of_text|>", "<|eot_id|>"):
        tid = tokenizer.token_to_id(name)
        if tid is not None:
            eos_ids.add(tid)

    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg = json.load(f)
        bos_id = cfg.get("bos_token_id", bos_id)
        raw_eos = cfg.get("eos_token_id", [])
        if isinstance(raw_eos, int):
            eos_ids.add(raw_eos)
        else:
            eos_ids.update(raw_eos)

    eos_ids.discard(None)
    return bos_id, eos_ids


# ---------------------------------------------------------------------------
# Stat line parsing
# ---------------------------------------------------------------------------

# Matches: "Generated token: 42 (cache=7) [matmul=82ms norm=4ms sampling=18ms]"
_DIAG_RE = re.compile(
    r"Generated token:\s*(\d+)"
    r".*?cache=(\d+)"
    r".*?matmul=(\d+)ms"
    r".*?norm=(\d+)ms"
    r".*?sampling=(\d+)ms"
)
# Matches: "Next predicted token: 42"
_TOKEN_RE = re.compile(r"Next predicted token:\s*(\d+)")


def _format_status(n_tokens: int, elapsed_wall: float,
                   matmul_ms: int, norm_ms: int, sampling_ms: int,
                   cache_size: int) -> str:
    tps = n_tokens / elapsed_wall if elapsed_wall > 0 else 0.0
    total_ms = matmul_ms + norm_ms + sampling_ms
    return dim(
        f"  [{tps:.1f} tok/s | "
        f"total={total_ms}ms matmul={matmul_ms}ms "
        f"norm={norm_ms}ms logits={sampling_ms}ms "
        f"cache={cache_size}]"
    )


# ---------------------------------------------------------------------------
# Main generation loop
# ---------------------------------------------------------------------------

def run_generation(tokenizer, history: list[int], bos_id: int,
                   eos_ids: set[int], max_new_tokens: int,
                   temp: float, top_p: float, penalty: float,
                   model_path: str) -> list[int]:
    """
    Spawn run_bitnet, stream tokens back, return list of generated token ids.
    """
    cmd = [
        RUN_BITNET_EXE,
        "--dir",     WEIGHTS_DIR,
        "--model",   model_path,
        "--tokens",  str(max_new_tokens),
        "--temp",    str(temp),
        "--top_p",   str(top_p),
        "--penalty", str(penalty),
    ] + [str(tid) for tid in history]

    try:
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            universal_newlines=True,
        )
    except FileNotFoundError:
        print(red(f"\n[Error] run_bitnet not found at: {RUN_BITNET_EXE}"))
        print(red("  Build with:  cmake --build build --config Release"))
        return []

    generated_ids: list[int] = []
    last_diag: dict = {}        # matmul_ms, norm_ms, sampling_ms, cache
    gen_start = time.monotonic()

    print(f"\n{bold('Model:')} ", end="", flush=True)

    in_stats = False

    while True:
        line = process.stdout.readline()
        if not line and process.poll() is not None:
            break
        if not line:
            continue

        stripped = line.rstrip("\r\n")

        # --- Stats block ---
        if "=== Generation Stats ===" in stripped:
            in_stats = True
            print(f"\n{dim('─' * 50)}")
            continue
        if in_stats:
            if "========================" in stripped:
                in_stats = False
                print(dim('─' * 50))
            else:
                print(dim("  " + stripped.strip()))
            continue

        # --- Diagnostic line (timing) ---
        m = _DIAG_RE.search(stripped)
        if m:
            last_diag = {
                "cache":    int(m.group(2)),
                "matmul":   int(m.group(3)),
                "norm":     int(m.group(4)),
                "sampling": int(m.group(5)),
            }
            continue  # don't print; absorb silently

        # --- Token line ---
        m = _TOKEN_RE.search(stripped)
        if m:
            next_token = int(m.group(1))
            generated_ids.append(next_token)

            # Decode and print immediately
            decoded = tokenizer.decode([next_token], skip_special_tokens=True)
            print(decoded, end="", flush=True)

            # Print status every 10 tokens or on EOS
            n = len(generated_ids)
            if n % 10 == 0 or next_token in eos_ids:
                elapsed = time.monotonic() - gen_start
                status = _format_status(
                    n, elapsed,
                    last_diag.get("matmul", 0),
                    last_diag.get("norm", 0),
                    last_diag.get("sampling", 0),
                    last_diag.get("cache", 0),
                )
                print(f"\n{status}", end="", flush=True)

            if next_token in eos_ids:
                print()
                break
            continue

        # --- Prefill / init messages (show dimmed) ---
        # Filter noisy internal lines; show only meaningful progress
        if any(kw in stripped for kw in (
            "Loading BF16",
            "Prefilling",
            "Workers:",
            "Manifest:",
            "Init failed",
            "Forward pass failed",
            "[Stop token",
        )):
            print(f"\n{dim(stripped.strip())}", end="", flush=True)

    # Collect stderr (only shown on error)
    stderr = process.stderr.read()
    if process.returncode not in (None, 0) and not generated_ids:
        print(red(f"\n[Process exited {process.returncode}]"))
        if stderr.strip():
            for ln in stderr.splitlines():
                print(red(f"  {ln}"))

    print()  # newline after response
    return generated_ids


# ---------------------------------------------------------------------------
# Chat loop
# ---------------------------------------------------------------------------

def main():
    # Verify exe exists before loading tokenizer
    if not os.path.exists(RUN_BITNET_EXE):
        print(red(f"[Error] Executable not found: {RUN_BITNET_EXE}"))
        print(      "  Build with: cmake --build build --config Release")
        sys.exit(1)

    if not os.path.exists(TOKENIZER_PATH):
        print(red(f"[Error] Tokenizer not found: {TOKENIZER_PATH}"))
        sys.exit(1)

    model_path = os.path.join(BASE_DIR, "model", "model.safetensors")
    if not os.path.exists(model_path):
        print(red(f"[Error] Safetensors not found: {model_path}"))
        sys.exit(1)

    print("Loading tokenizer...", end="", flush=True)
    tokenizer = Tokenizer.from_file(TOKENIZER_PATH)
    bos_id, eos_ids = load_token_ids(tokenizer)
    print(f" done  (bos={bos_id}, eos={sorted(eos_ids)})")

    print(f"\n{bold('=== BitNet b1.58 Chat ===')}  (type {dim('exit')} to quit)\n")

    history: list[int] = [bos_id]

    while True:
        try:
            user_input = input(f"{cyan('User:')} ")
        except (EOFError, KeyboardInterrupt):
            print("\nBye.")
            break

        if user_input.strip().lower() in ("exit", "quit", "q"):
            print("Bye.")
            break

        if not user_input.strip():
            continue

        # Encode user turn
        encoded = tokenizer.encode(user_input, add_special_tokens=False)
        history.extend(encoded.ids)

        # Generate
        new_ids = run_generation(
            tokenizer, history, bos_id, eos_ids,
            max_new_tokens = DEFAULT_MAX_NEW_TOKENS,
            temp           = DEFAULT_TEMPERATURE,
            top_p          = DEFAULT_TOP_P,
            penalty        = DEFAULT_PENALTY,
            model_path     = model_path,
        )

        # Append model response to history for next turn
        history.extend(new_ids)


if __name__ == "__main__":
    main()
