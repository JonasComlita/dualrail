from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from tools import trit_tool
from tools.treatcode_platform import test_schema_fixtures, validate_schema_catalog


REPO = Path(__file__).resolve().parents[2]
MANIFEST_PATH = REPO / "TREATCODE_PLAN_MANIFEST.json"


class TreatCodePlanVerifierTests(unittest.TestCase):
    def load_manifest(self) -> dict:
        return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    def validate_manifest(self, manifest: dict) -> dict:
        return trit_tool.validate_treatcode_plan_manifest(manifest)

    def test_schema_catalog_and_fixtures_pass(self) -> None:
        catalog = validate_schema_catalog()
        self.assertTrue(catalog["ok"], catalog)
        fixtures = test_schema_fixtures()
        self.assertTrue(fixtures["ok"], fixtures)
        self.assertEqual(
            {item["expected"] for item in fixtures["fixtures"]},
            {"valid", "invalid"},
        )

    def test_manifest_covers_index(self) -> None:
        report = self.validate_manifest(self.load_manifest())
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["plan_count"], 14)

    def test_missing_dependency_is_rejected(self) -> None:
        manifest = self.load_manifest()
        next(item for item in manifest["plans"] if item["id"] == "P01")["depends_on"] = ["P99"]
        report = self.validate_manifest(manifest)
        self.assertFalse(report["ok"])
        self.assertIn("missing_dependency", {item["code"] for item in report["errors"]})

    def test_dependency_cycle_is_rejected(self) -> None:
        manifest = self.load_manifest()
        next(item for item in manifest["plans"] if item["id"] == "P00")["depends_on"] = ["P01"]
        next(item for item in manifest["plans"] if item["id"] == "P01")["depends_on"] = ["P00"]
        report = self.validate_manifest(manifest)
        self.assertFalse(report["ok"])
        self.assertIn("dependency_cycle", {item["code"] for item in report["errors"]})

    def test_duplicate_id_is_rejected(self) -> None:
        manifest = self.load_manifest()
        manifest["plans"].append(copy.deepcopy(manifest["plans"][0]))
        report = self.validate_manifest(manifest)
        self.assertFalse(report["ok"])
        self.assertIn("duplicate_plan_id", {item["code"] for item in report["errors"]})

    def test_invalid_status_is_rejected(self) -> None:
        manifest = self.load_manifest()
        next(item for item in manifest["plans"] if item["id"] == "P01")["status"] = "finished"
        report = self.validate_manifest(manifest)
        self.assertFalse(report["ok"])
        self.assertIn("invalid_status", {item["code"] for item in report["errors"]})

    def test_missing_commands_and_evidence_are_rejected(self) -> None:
        manifest = self.load_manifest()
        p01 = next(item for item in manifest["plans"] if item["id"] == "P01")
        p01["verification_commands"] = []
        p01["evidence"] = []
        report = self.validate_manifest(manifest)
        self.assertFalse(report["ok"])
        codes = {item["code"] for item in report["errors"]}
        self.assertIn("absent_verification_commands", codes)
        self.assertIn("absent_evidence_references", codes)

    def verify_temp_manifest(self, manifest: dict) -> dict:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest["schema_file"] = str(REPO / "TREATCODE_PLAN_MANIFEST_SCHEMA.json")
            manifest["index"] = str(REPO / "docs" / "11_TreatCode_Platform" / "PLAN_INDEX.md")
            path = root / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            return trit_tool.verify_treatcode_plan(
                "P01",
                manifest_path=path,
                output_dir=root / "evidence",
                repo_root=REPO,
            )

    def test_failing_command_cannot_verify_complete(self) -> None:
        manifest = self.load_manifest()
        p01 = next(item for item in manifest["plans"] if item["id"] == "P01")
        p01["verification_commands"] = [
            {"id": "fail", "command": "python -c \"import sys; sys.exit(1)\""},
            {"id": "self-verify", "command": "python tools/trit_tool.py website plan verify P01"},
        ]
        report = self.verify_temp_manifest(manifest)
        self.assertFalse(report["complete"])
        self.assertIn("verification_command_failed", {item["code"] for item in report["issues"]})

    def test_missing_human_approval_blocks_completion(self) -> None:
        manifest = self.load_manifest()
        p00 = next(item for item in manifest["plans"] if item["id"] == "P00")
        p01 = next(item for item in manifest["plans"] if item["id"] == "P01")
        p00["status"] = "complete"
        p00["completion_record"] = {
            "verified_commit": "d168bc845babad7d6031bed98a16e3a471d200c5",
            "evidence_artifact": "build/treatcode-plan-evidence/P00/result.json",
            "date": "2026-07-31T00:00:00Z",
            "human_approvals": [{"role": "Tooling maintainer", "reviewer": "reviewer", "decision": "approved", "date": "2026-07-31T00:00:00Z", "commit": "d168bc845babad7d6031bed98a16e3a471d200c5"}],
        }
        p01["status"] = "complete"
        p01["verification_commands"] = [{"id": "self-verify", "command": "python tools/trit_tool.py website plan verify P01"}]
        p01["completion_record"] = {
            "verified_commit": "d168bc845babad7d6031bed98a16e3a471d200c5",
            "evidence_artifact": "build/treatcode-plan-evidence/P01/result.json",
            "date": "2026-07-31T00:00:00Z",
        }
        report = self.verify_temp_manifest(manifest)
        self.assertFalse(report["complete"])
        self.assertIn("human_approval_missing", {item["code"] for item in report["issues"]})


if __name__ == "__main__":
    unittest.main()
