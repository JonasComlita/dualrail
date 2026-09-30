#!/usr/bin/env python3
"""Small, current-platform validation CLI.

This is the supported repository entry point. It deliberately has no legacy
image migration, wrapper-script, or deleted-tree compatibility commands.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
ARCHITECTURE = ROOT / "ARCHITECTURE_MANIFEST.json"
IMAGE_FORMAT = ROOT / "IMAGE_FORMAT_MANIFEST.json"
BUILD_DIR = ROOT / "build_current_cleanup"


def _load(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def doctor() -> int:
    required = [
        ROOT / "CMakeLists.txt",
        ARCHITECTURE,
        IMAGE_FORMAT,
        ROOT / "architecture_contract.h",
        ROOT / "tests" / "current_only_conformance.cpp",
    ]
    missing = [str(path.relative_to(ROOT)) for path in required if not path.exists()]
    if missing:
        print("missing current-platform files:")
        print("\n".join(f"- {item}" for item in missing))
        return 1
    print("current platform files: OK")
    return 0


def check_contracts() -> int:
    architecture = _load(ARCHITECTURE)
    formats = _load(IMAGE_FORMAT)
    errors: list[str] = []
    if architecture["architecture"]["isa_version"] != 2:
        errors.append("architecture.isa_version must be 2")
    abi = architecture["abi"]
    if (
        abi["executable_version"] != 3
        or abi["function_abi_version"] != 3
        or abi["syscall_abi_version"] != 2
        or abi["vector_abi_version"] != 1
    ):
        errors.append(
            "architecture.abi must be executable/function ABI v3, syscall ABI v2, and vector ABI 1"
        )
    image = formats["formats"]
    if image["tboot"]["write_version"] != 3 or image["tboot"]["read_versions"] != [3]:
        errors.append("tboot must be v3 write/read only")
    if image["tdisk"]["write_version"] != 2 or image["tdisk"]["read_versions"] != [2]:
        errors.append("tdisk must be v2 write/read only")
    for name, payload in image.items():
        if any("migration" in key for key in payload):
            errors.append(f"{name} contains an offline migration field")
    if errors:
        print("contract errors:")
        print("\n".join(f"- {error}" for error in errors))
        return 1
    print("current platform contracts: OK")
    return 0


def run_test() -> int:
    configure = subprocess.run(
        ["cmake", "-S", str(ROOT), "-B", str(BUILD_DIR)], check=False
    )
    if configure.returncode:
        return configure.returncode
    return subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "current_validate"],
        check=False,
    ).returncode


def export_diagnostics() -> int:
    output = BUILD_DIR / "current-platform-diagnostics.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(
            {
                "platform": "current-only",
                "isa_version": 2,
                "executable_abi_version": 3,
                "tboot_version": 3,
                "tdisk_version": 2,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(output)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="trit_tool.py")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("doctor")
    subparsers.add_parser("contract-check")
    subparsers.add_parser("test")
    subparsers.add_parser("export-diagnostics")
    args = parser.parse_args(argv)
    return {
        "doctor": doctor,
        "contract-check": check_contracts,
        "test": run_test,
        "export-diagnostics": export_diagnostics,
    }[args.command]()


if __name__ == "__main__":
    sys.exit(main())
