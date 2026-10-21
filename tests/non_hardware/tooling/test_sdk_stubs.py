"""Check that the optional SDK integration generator fails closed without an SDK."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GENERATOR = Path(__file__).resolve().parents[2] / "sdk_integration" / "generate_stubs.py"


class SdkStubTests(unittest.TestCase):
    def generate(self, required, defined, header):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        (root / "sidekiq_api.h").write_text(header)
        nm = root / "nm"
        nm.write_text(
            f"#!{sys.executable}\nimport sys\n"
            f"print({required!r} if sys.argv[1] == '--undefined-only' else {defined!r})\n"
        )
        nm.chmod(0o755)
        output = root / "stubs.cpp"
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--nm", str(nm), "--output", str(output),
             "--sdk-headers", str(root), "--objects", str(root / "unused.o")],
            text=True, capture_output=True,
        )
        return result, output

    def test_only_unimplemented_symbols_receive_abort_stubs(self):
        result, output = self.generate(
            " U skiq_scripted\n U skiq_pending", " T skiq_scripted",
            "EPIQ_API int32_t skiq_scripted(int count);\n"
            "EPIQ_API const char *skiq_pending(uint8_t card, void (*callback)(int));\n",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        source = output.read_text()
        self.assertNotIn("skiq_scripted", source)
        self.assertIn("const char * skiq_pending(uint8_t card, void (*callback)(int))", source)
        self.assertIn("std::abort();", source)
        self.assertNotIn("dlopen", source)

    def test_unknown_sdk_declaration_fails_without_output(self):
        result, output = self.generate(" U skiq_unknown", "", "/* no matching declaration */")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK prototypes missing for: skiq_unknown", result.stderr)
        self.assertFalse(output.exists())

    def test_fully_scripted_symbols_need_no_fallback_definitions(self):
        result, output = self.generate(" U skiq_scripted", " T skiq_scripted", "")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Generated 0 fail-fast SDK definitions", result.stdout)
        self.assertNotIn("std::abort();", output.read_text())


if __name__ == "__main__":
    unittest.main()
