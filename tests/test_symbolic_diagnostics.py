"""Golden-test opt-in symbolic projections for runtime diagnostics."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from trit_tool import _add_symbolic_fields, _apply_symbolic_diagnostics  # noqa: E402


def main() -> int:
    image_report: dict[str, object] = {}
    _add_symbolic_fields(image_report, {"image.boot_entry": 7}, "all", "t50")
    assert image_report["symbolic"]["declared_width"] == 50  # type: ignore[index]
    image_payload = image_report["symbolic"]["fields"]["image.boot_entry"]  # type: ignore[index]
    assert image_payload["declared_width"] == 50  # type: ignore[index]
    assert image_payload["view"] == "t50"  # type: ignore[index]

    with tempfile.TemporaryDirectory(prefix="trit-symbolic-diagnostics-") as raw:
        root = Path(raw)
        (root / "manifest.json").write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "runtime": {"pc": 7, "cycles": 12, "status": "HALTED"},
                    "processes": [{"pid": 3, "context_epc": 42}],
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (root / "vm_state.txt").write_text(
            "pc=7\nstatus=HALTED\nr1=-42\nsp=81\n", encoding="utf-8"
        )
        (root / "syscall_trace.jsonl").write_text(
            '{"schema":"trit.syscall_trace.v1","pc":7,"syscall_id":4}\n',
            encoding="utf-8",
        )

        result = _apply_symbolic_diagnostics(root, "all", "t40")
        assert result["declared_width"] == 40

        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["runtime"]["pc"] == 7
        payload = manifest["symbolic"]["fields"]["runtime.pc"]
        assert payload["value"] == 7
        assert payload["declared_width"] == 40
        assert payload["view"] == "t40"
        assert set(payload["formats"]) == {"decimal", "hex", "trit", "base27", "base81"}

        trace = json.loads((root / "syscall_trace.jsonl").read_text(encoding="utf-8"))
        assert trace["pc"] == 7
        assert trace["symbolic"]["fields"]["syscall_id"]["value"] == 4

        symbolic_text = (root / "vm_state.symbolic.txt").read_text(encoding="utf-8")
        assert "format=all" in symbolic_text
        assert "view=t40" in symbolic_text
        assert "r1=decimal=-42" in symbolic_text

    print("symbolic diagnostics: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
