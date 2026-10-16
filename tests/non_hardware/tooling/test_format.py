"""Exercise the formatting gate with real Git diffs and clang-format 14."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which("clang-format-14"), "clang-format-14 is not installed")
class FormatTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="format check ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "tools").mkdir()
        shutil.copy(REPO / "tools/check_format.py", self.root / "tools/check_format.py")
        shutil.copy(REPO / ".clang-format", self.root / ".clang-format")
        # Leave unrelated inherited layout debt in the initial commit.
        (self.root / "old.cpp").write_text("int  old(){return 1;}\n")
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Format Test", "-c", "user.email=format@example.invalid",
                 "commit", "--quiet", "-m", "fixture")

    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

    def check_format(self):
        return subprocess.run(
            [sys.executable, str(self.root / "tools/check_format.py"), "--base", "HEAD"],
            # Invocation outside the checkout must still check the right tree.
            cwd=self.root.parent, capture_output=True, text=True,
        )

    def test_unchanged_legacy_layout_does_not_fail(self):
        result = self.check_format()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0 C++ files", result.stdout)

    def test_unformatted_added_file_fails_with_diff(self):
        (self.root / "new file.cpp").write_text("int  added(){return 2;}\n")
        self.git("add", "new file.cpp")
        result = self.check_format()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("new file.cpp (formatted)", result.stdout)

    def test_formatted_added_file_passes(self):
        (self.root / "new.cpp").write_text("int added()\n{\n    return 2;\n}\n")
        self.git("add", "new.cpp")
        result = self.check_format()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 C++ files", result.stdout)

    def test_unstaged_edit_is_checked(self):
        (self.root / "old.cpp").write_text("int  old(){return 5;}\n")
        result = self.check_format()
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_deleted_file_is_ignored(self):
        (self.root / "old.cpp").unlink()
        result = self.check_format()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_config_fails_without_cpp_changes(self):
        (self.root / ".clang-format").write_text("NotAFormattingOption: true\n")
        result = self.check_format()
        self.assertEqual(result.returncode, 2, result.stdout)

    def test_unknown_base_fails(self):
        result = subprocess.run(
            [sys.executable, str(self.root / "tools/check_format.py"),
             "--base", "refs/heads/does-not-exist"], capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
