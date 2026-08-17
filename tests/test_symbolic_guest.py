"""Execute the allocation-free guest symbolic SDK in the native VM.

This is intentionally an executable acceptance test.  The probe exercises the
general integer quotient intrinsic, the complete TASCII-81 table, canonical
formatters, all parser prefixes, error paths, and caller-buffer invariants.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

TRITC = ROOT / "build" / "tritc.exe"
SDK = ROOT / "apps" / "os_sdk.trit"

from trit_symbolic import (  # noqa: E402
    TASCII81_TABLE,
    balanced_trits,
    format_base27,
    format_base81,
    format_trit_literal,
)


def _check_buffer(address: int, text: str, code: int) -> list[str]:
    checks: list[str] = []
    for offset, char in enumerate(text):
        checks.append(
            f"if os_symbolic_load({address}, {offset}) != {ord(char)} {{ return {code}; }}"
        )
    checks.append(
        f"if os_symbolic_load({address}, {len(text)}) != 0 {{ return {code}; }}"
    )
    return checks


def _seed(address: int, count: int, value: int = 777777) -> list[str]:
    return [f"os_symbolic_store({address}, {offset}, {value});" for offset in range(count)]


def _build_probe(sdk: str) -> str:
    lines = [
        "fn main() -> t40 {",
        "    var x: t40 = 42;",
        "    while x > 0 { x = tdiv(x, 10); }",
        "    if x != 0 { return 10; }",
        "    var y: t40 = -42;",
        "    while y < 0 { y = tdiv(y, 10); }",
        "    if y != 0 { return 11; }",
    ]

    # Cross-language table parity: every host table character must round-trip
    # through both guest single-symbol entry points.
    for index, char in enumerate(TASCII81_TABLE):
        lines.append(
            f"    if os_tascii81_encode_char({ord(char)}) != {index} {{ return {100 + index}; }}"
        )
        lines.append(
            f"    if os_tascii81_decode_char({index}) != {ord(char)} {{ return {200 + index}; }}"
        )
    lines += [
        "    if os_tascii81_encode_char(64) != -1 { return 300; }",
        "    if os_tascii81_decode_char(81) != -1 { return 301; }",
    ]

    # Canonical formatter output, including the terminating NUL and exact
    # capacity contract.  The host module supplies the expected spellings.
    values = (-10000000, -42, -1, 0, 1, 42, 10000000)
    address = 3000
    for value in values:
        expected = {
            0: format_trit_literal(balanced_trits(value)),
            1: format_base27(value),
            2: format_base81(value),
        }
        for selector, text in expected.items():
            lines.append(
                f"    var formatted: t40 = os_symbolic_format({value}, {selector}, {address}, 80);"
            )
            lines.append(
                f"    if formatted != {len(text)} {{ return {400 + len(lines)}; }}"
            )
            lines += [f"    {check}" for check in _check_buffer(address, text, 500 + len(lines))]
            address += 100
        for selector, text in ((3, f"0x{value & ((1 << 64) - 1):X}"), (4, str(value))):
            lines.append(
                f"    var formatted_numeric: t40 = os_symbolic_format({value}, {selector}, {address}, 80);"
            )
            lines.append(
                f"    if formatted_numeric != {len(text)} {{ return {700 + len(lines)}; }}"
            )
            lines += [f"    {check}" for check in _check_buffer(address, text, 800 + len(lines))]
            address += 100

    # Buffer errors and invalid selectors must be non-mutating.
    lines += [f"    {statement}" for statement in _seed(9000, 8)]
    lines += [
        "    if os_symbolic_format(42, 4, 9000, 2) != -3 { return 1000; }",
    ]
    lines += [f"    if os_symbolic_load(9000, {i}) != 777777 {{ return {1001 + i}; }}" for i in range(8)]
    lines += [
        "    if os_symbolic_format(42, 99, 9000, 8) != -4 { return 1010; }",
    ]
    lines += [f"    if os_symbolic_load(9000, {i}) != 777777 {{ return {1011 + i}; }}" for i in range(8)]

    # Raw TASCII buffers are length based and also reject malformed input
    # without partially writing the destination.
    lines += [
        "    os_symbolic_store(9100, 0, 32);",
        "    os_symbolic_store(9100, 1, 65);",
        "    os_symbolic_store(9100, 2, 64);",
        "    os_symbolic_store(9200, 0, 777777);",
        "    os_symbolic_store(9200, 1, 777777);",
        "    if os_tascii81_encode(9100, 3, 9200, 3) != -1 { return 1020; }",
        "    if os_symbolic_load(9200, 0) != 777777 { return 1021; }",
        "    if os_symbolic_load(9200, 1) != 777777 { return 1022; }",
        "    if os_tascii81_encode(9100, 2, 9200, 1) != -3 { return 1023; }",
    ]

    # Numeric parser parity for decimal/hex/exact/compatibility/compact
    # spellings, plus malformed, overflow, and no-write buffer behavior.
    parser_cases = [
        ("42", 42),
        ("-42", -42),
        ("0x2A", 42),
        ("0X2a", 42),
        ("0t+---0", 42),
        ("0y+---0", 42),
        ("0z27:f1", 42),
        ("0z81:f1", 42),
    ]
    source_address = 10000
    output_address = 11000
    for case_index, (literal, expected) in enumerate(parser_cases):
        lines.append(f"    os_symbolic_store({source_address}, 0, 777777);")
        lines += [
            f"    os_symbolic_store({source_address}, {offset}, {ord(char)});"
            for offset, char in enumerate(literal)
        ]
        lines += [
            f"    os_symbolic_store({output_address}, 0, 777777);",
            f"    if os_symbolic_parse_numeric({source_address}, {len(literal)}, {output_address}) != 0 {{ return {1100 + case_index}; }}",
            f"    if os_symbolic_load({output_address}, 0) != {expected} {{ return {1120 + case_index}; }}",
        ]
        parsed_trits = balanced_trits(expected)
        lines.append(
            f"    if os_symbolic_parse({source_address}, {len(literal)}, {output_address + 100}, {len(parsed_trits)}) != {len(parsed_trits)} {{ return {1140 + case_index}; }}"
        )
        for offset, trit in enumerate(parsed_trits):
            lines.append(
                f"    if os_symbolic_load({output_address + 100}, {offset}) != {trit} {{ return {1160 + case_index}; }}"
            )
        source_address += 100
        output_address += 100

    malformed = ("", "0t", "0t+?", "0z27:", "0z27:@", "0z81:~", "0x", "12x")
    for case_index, literal in enumerate(malformed):
        address = 13000 + case_index * 10
        lines.append(f"    os_symbolic_store({address}, 0, 777777);")
        lines += [
            f"    os_symbolic_store({address}, {offset}, {ord(char)});"
            for offset, char in enumerate(literal)
        ]
        lines += [
            "    os_symbolic_store(14000, 0, 777777);",
            f"    if os_symbolic_parse_numeric({address}, {len(literal)}, 14000) >= 0 {{ return {1200 + case_index}; }}",
            f"    if os_symbolic_load(14000, 0) != 777777 {{ return {1220 + case_index}; }}",
        ]

    overflow = "10000001"
    lines += [
        "    os_symbolic_store(15000, 0, 777777);",
        *[
            f"    os_symbolic_store(15000, {offset}, {ord(char)});"
            for offset, char in enumerate(overflow)
        ],
        f"    if os_symbolic_parse_numeric(15000, {len(overflow)}, 15001) != -2 {{ return 1250; }}",
        f"    if os_symbolic_format(10000001, 4, 15002, 32) != -2 {{ return 1251; }}",
    ]

    # An undersized parse destination is checked after validation and does not
    # touch any word, including the first word.
    lines += [
        "    os_symbolic_store(16000, 0, 52);",
        "    os_symbolic_store(16000, 1, 50);",
        "    os_symbolic_store(17000, 0, 777777);",
        "    os_symbolic_store(17000, 1, 777777);",
        "    os_symbolic_store(17000, 2, 777777);",
        "    if os_symbolic_parse(16000, 2, 17000, 1) != -3 { return 1260; }",
        "    if os_symbolic_load(17000, 0) != 777777 { return 1261; }",
        "    if os_symbolic_load(17000, 1) != 777777 { return 1262; }",
        "    if os_symbolic_load(17000, 2) != 777777 { return 1263; }",
        "    return 0;",
        "}",
        "",
        sdk,
    ]
    return "\n".join(lines)


def main() -> int:
    if not TRITC.exists():
        print(f"missing compiler: {TRITC}")
        return 1

    sdk = SDK.read_text(encoding="utf-8")
    required = (
        "fn os_tascii81_encode_char",
        "fn os_tascii81_decode_char",
        "fn os_symbolic_parse(",
        "fn os_symbolic_format(",
        "OS_SYMBOLIC_PARSE_VALUE_BIAS",
        "OS_SYMBOLIC_FORMAT_TRIT",
        "OS_SYMBOLIC_FORMAT_BASE27",
        "OS_SYMBOLIC_FORMAT_BASE81",
        "OS_SYMBOLIC_FORMAT_HEX",
        "OS_SYMBOLIC_FORMAT_DECIMAL",
    )
    missing = [name for name in required if name not in sdk]
    if missing:
        print("missing guest SDK symbols:", ", ".join(missing))
        return 1

    source_path: Path | None = None
    with tempfile.NamedTemporaryFile(
        mode="w",
        suffix=".trit",
        prefix="symbolic-guest-runtime-",
        delete=False,
        encoding="utf-8",
    ) as source_file:
        source_file.write(_build_probe(sdk))
        source_path = Path(source_file.name)

    try:
        result = subprocess.run(
            [str(TRITC), "run", str(source_path), "-O1", "--no-ansi", "--steps", "2000000"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=180,
            check=False,
        )
    finally:
        if source_path is not None:
            source_path.unlink(missing_ok=True)

    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return result.returncode or 1

    if re.search(r"Final CPU Status:\s+HALTED", result.stdout) is None:
        print(result.stdout)
        print("guest symbolic probe did not halt")
        return 1
    match = re.search(r"Return Register r13:\s*(-?\d+)", result.stdout)
    if match is None:
        print(result.stdout)
        print("guest symbolic probe did not report r13")
        return 1
    if int(match.group(1)) != 0:
        print(result.stdout)
        print(f"guest symbolic probe failed with code {match.group(1)}")
        return 1

    print("Guest symbolic SDK executes in the VM")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
