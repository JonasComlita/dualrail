from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.repository_ingestion import (
    INDEX_FILENAME,
    build_index,
    generate_context_package,
    verify_index,
)


class RepositoryIngestionTests(unittest.TestCase):
    def make_repository(self) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="treatcode-index-fixture-"))
        files = {
            "src/a.py": "from src.b import b\n\ndef a():\n    return b()\n",
            "src/b.py": "def b():\n    return 1\n",
            "tests/test_a.py": "from src.a import a\nassert a() == 1\n",
            "benchmarks/bench.py": "from src.a import a\nprint(a())\n",
            "docs/guide.md": "[source](../src/a.py)\n",
            "generated/output.py": "def generated():\n    return 0\n",
            "manifest.json": json.dumps(
                {
                    "schema": "fixture",
                    "source_refs": [{"repository": "fixture", "commit": "abcdef1", "path": "src/a.py", "status": "resolved"}],
                    "test_refs": ["tests/test_a.py"],
                }
            )
            + "\n",
        }
        for relative, content in files.items():
            path = directory / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        subprocess.run(["git", "init"], cwd=directory, check=True, capture_output=True)
        subprocess.run(["git", "config", "user.email", "test@example.invalid"], cwd=directory, check=True)
        subprocess.run(["git", "config", "user.name", "TreatCode test"], cwd=directory, check=True)
        subprocess.run(["git", "add", "."], cwd=directory, check=True)
        subprocess.run(["git", "commit", "-m", "fixture"], cwd=directory, check=True, capture_output=True)
        return directory

    def test_clean_incremental_and_provenance(self) -> None:
        repository = self.make_repository()
        try:
            output = repository / "index"
            clean = build_index(repo_root=repository, output_dir=output, clean=True)
            incremental = build_index(repo_root=repository, output_dir=output, clean=False)
            self.assertEqual(clean["index_sha256"], incremental["index_sha256"])

            index = json.loads((output / INDEX_FILENAME).read_text(encoding="utf-8"))
            self.assertEqual(len(index["files"]), 7)
            self.assertTrue(any(item["type"] == "imports" for item in index["relationships"]))
            self.assertTrue(any(item["type"] == "calls" and item["resolution"] == "resolved" for item in index["relationships"]))
            self.assertTrue(any(item["type"] == "tests" for item in index["relationships"]))
            self.assertTrue(any(item["type"] == "benchmarks" for item in index["relationships"]))
            generated = next(item for item in index["files"] if item["path"] == "generated/output.py")
            self.assertFalse(generated["source_authority"])
            self.assertEqual(verify_index(repo_root=repository, index_path=output)["ok"], True)
        finally:
            import shutil

            shutil.rmtree(repository, ignore_errors=True)

    def test_context_package_is_bounded_and_hashed(self) -> None:
        repository = self.make_repository()
        try:
            output = repository / "index"
            build_index(repo_root=repository, output_dir=output, clean=True)
            index = json.loads((output / INDEX_FILENAME).read_text(encoding="utf-8"))
            package = generate_context_package(index, ["src/a.py"], repo_root=repository)
            self.assertEqual(package["schema"], "treatcode.context-package.v1")
            self.assertTrue(package["selection"]["bounded"])
            self.assertTrue(package["package_sha256"].startswith("sha256:"))
            paths = {item["path"] for item in package["files"]}
            self.assertIn("src/a.py", paths)
            self.assertIn("src/b.py", paths)
            self.assertNotIn("generated/output.py", paths)
        finally:
            import shutil

            shutil.rmtree(repository, ignore_errors=True)

    def test_dirty_tracked_worktree_cannot_be_fresh_commit_index(self) -> None:
        repository = self.make_repository()
        try:
            tracked_file = repository / "src/a.py"
            tracked_file.write_text("from src.b import b\n\ndef a():\n    return b() + 1\n", encoding="utf-8")
            output = repository / "index"

            # Build after the edit: the old implementation indexed the dirty
            # worktree bytes while labeling every source reference with HEAD.
            build_index(repo_root=repository, output_dir=output, clean=True)
            report = verify_index(repo_root=repository, index_path=output)

            self.assertFalse(report["ok"])
            self.assertFalse(report["freshness"]["fresh"])
            self.assertIn("src/a.py", report["freshness"]["dirty_tracked_files"])
        finally:
            import shutil

            shutil.rmtree(repository, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
