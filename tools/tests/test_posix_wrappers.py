"""Static and optional Bash smoke checks for the POSIX host-tool wrappers."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
EXPECTED_COMMANDS = {
    "trit-bench.sh": "bench",
    "trit-build-image.sh": "build-image",
    "trit-compact-disk.sh": "compact-disk",
    "trit-doctor.sh": "doctor",
    "trit-export-diagnostics.sh": "export-diagnostics",
    "trit-fuzz.sh": "fuzz",
    "trit-inspect-image.sh": "inspect-image",
    "trit-knowledge.sh": "knowledge",
    "trit-replay.sh": "replay",
    "trit-run.sh": "run",
    "trit-test.sh": "test",
}


def usable_bash() -> str | None:
    """Return a working Bash executable, or None on Windows without WSL."""

    bash = shutil.which("bash")
    if not bash:
        return None
    try:
        result = subprocess.run(
            [bash, "-c", "exit 0"],
            capture_output=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return bash if result.returncode == 0 else None


class PosixWrapperTests(unittest.TestCase):
    def test_inventory_and_shell_contract(self) -> None:
        self.assertEqual(
            set(EXPECTED_COMMANDS),
            {path.name for path in TOOLS.glob("trit-*.sh") if path.name != "trit-posix.sh"},
        )
        helper = (TOOLS / "trit-posix.sh").read_text(encoding="utf-8")
        self.assertTrue(helper.startswith("#!/usr/bin/env bash\n"))
        self.assertIn("set -euo pipefail", helper)
        self.assertIn("BASH_SOURCE", helper)
        self.assertIn("TRIT_BUILD_DIR", helper)
        self.assertNotIn("eval ", helper)
        self.assertIn('exec "$trit_posix_python" "$trit_posix_tool"', helper)

        for name, command in EXPECTED_COMMANDS.items():
            text = (TOOLS / name).read_text(encoding="utf-8")
            self.assertTrue(text.startswith("#!/usr/bin/env bash\n"), name)
            self.assertIn("set -euo pipefail", text, name)
            self.assertIn('trit-posix.sh"', text, name)
            self.assertIn(f'trit_posix_run {command} "$@"', text, name)
            self.assertNotIn("eval ", text, name)

    @unittest.skipUnless(shutil.which("bash"), "Bash is unavailable on this host")
    def test_help_paths_forward_through_each_wrapper(self) -> None:
        bash = usable_bash()
        if not bash:
            self.skipTest("Bash is installed as an unusable WSL shim")
        env = os.environ.copy()
        # Windows App Execution Aliases can shadow python3 in MSYS shells;
        # pin the interpreter so this smoke check exercises the wrapper.
        env["TRIT_PYTHON"] = sys.executable
        for name in EXPECTED_COMMANDS:
            result = subprocess.run(
                [bash, str(TOOLS / name), "--help"],
                cwd=REPO,
                env=env,
                text=True,
                capture_output=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"{name}: stdout={result.stdout!r} stderr={result.stderr!r}",
            )

    @unittest.skipUnless(shutil.which("bash"), "Bash is unavailable on this host")
    def test_argument_and_build_dir_forwarding_with_spaces(self) -> None:
        bash = usable_bash()
        if not bash:
            self.skipTest("Bash is installed as an unusable WSL shim")
        with tempfile.TemporaryDirectory(prefix="trit wrapper ") as temp:
            temp_path = Path(temp)
            capture = temp_path / "captured argv.txt"
            fake_python = temp_path / "fake python"
            fake_python.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"$TRIT_CAPTURE\"\n"
                "printf 'BUILD=%s\\n' \"${TRIT_BUILD_DIR-}\" >> \"$TRIT_CAPTURE\"\n",
                encoding="utf-8",
            )
            os.chmod(fake_python, 0o755)
            env = os.environ.copy()
            env["TRIT_PYTHON"] = str(fake_python)
            env["TRIT_CAPTURE"] = str(capture)
            env["TRIT_BUILD_DIR"] = str(temp_path / "custom build")
            # Keep one argument and one path containing spaces.  Embedded quote
            # escaping is covered by the static no-eval assertion; Windows'
            # MSYS command-line bridge cannot preserve literal quote characters
            # when Bash is launched through the Win32 process API.
            forwarded = ["--value with spaces", "path/with spaces"]
            space_tools = temp_path / "repo with spaces" / "tools"
            space_tools.mkdir(parents=True)
            shutil.copy2(TOOLS / "trit-posix.sh", space_tools / "trit-posix.sh")
            shutil.copy2(TOOLS / "trit-doctor.sh", space_tools / "trit-doctor.sh")
            (space_tools / "trit_tool.py").write_text("# capture stub\n", encoding="utf-8")
            result = subprocess.run(
                [bash, str(space_tools / "trit-doctor.sh"), *forwarded],
                cwd=temp_path,
                env=env,
                text=True,
                capture_output=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            captured = capture.read_text(encoding="utf-8").splitlines()
            self.assertEqual(Path(captured[0]).name, "trit_tool.py")
            self.assertEqual(captured[1:], ["doctor", *forwarded, f"BUILD={env['TRIT_BUILD_DIR']}"])


if __name__ == "__main__":
    unittest.main()
