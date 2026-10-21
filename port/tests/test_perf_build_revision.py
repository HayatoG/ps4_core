# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HELPER = Path(__file__).resolve().parents[1] / "gpu/cmake/PerfBuildRevision.cmake"


@unittest.skipUnless(
    shutil.which("git") and shutil.which("cmake"), "git and cmake required"
)
class BuildRevision(unittest.TestCase):
    def test_staged_and_unstaged_changes_are_dirty(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)

            def git(*args):
                return subprocess.run(
                    ["git", *args], cwd=root, check=True, capture_output=True, text=True
                ).stdout.strip()

            git("init", "-q")
            source = root / "source.txt"
            source.write_text("baseline", encoding="utf-8")
            git("add", "source.txt")
            git(
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "commit",
                "-qm",
                "fixture",
            )
            revision = git("rev-parse", "--short=12", "HEAD")
            script = root / "probe.cmake"
            result = root / "revision.txt"
            script.write_text(
                f'include("{HELPER.as_posix()}")\n'
                f'tlg_perf_build_revision("{root.as_posix()}" REV)\n'
                f'file(WRITE "{result.as_posix()}" "${{REV}}")\n',
                encoding="utf-8",
            )

            def identify():
                subprocess.run(
                    ["cmake", "-P", str(script)], check=True, capture_output=True
                )
                return result.read_text(encoding="utf-8")

            self.assertEqual(identify(), revision)
            source.write_text("diagnostic change", encoding="utf-8")
            self.assertEqual(identify(), revision + "-dirty")
            git("add", "source.txt")
            self.assertEqual(identify(), revision + "-dirty")

    def test_source_archive_is_unknown_not_dirty(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            script = root / "probe.cmake"
            result = root / "revision.txt"
            script.write_text(
                f'include("{HELPER.as_posix()}")\n'
                f'tlg_perf_build_revision("{root.as_posix()}" REV)\n'
                f'file(WRITE "{result.as_posix()}" "${{REV}}")\n',
                encoding="utf-8",
            )
            # Keep git from finding a repository above the temporary folder.
            env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(root.parent))
            subprocess.run(
                ["cmake", "-P", str(script)], check=True, capture_output=True, env=env
            )
            self.assertEqual(result.read_text(encoding="utf-8"), "unknown")


if __name__ == "__main__":
    unittest.main()
