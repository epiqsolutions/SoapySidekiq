"""Exercise install.sh argument handling without building or installing a driver."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[3]


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "isolated prefix"
        self.build = self.root / "build"
        self.log = self.root / "commands.jsonl"
        project = self.root / "project"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(InstallProbe NONE)\n")
        binary = self.root / "bin"
        binary.mkdir()
        # Only configure a tiny SDK-free project with real CMake. Build/install
        # and sudo are recorded; no driver or privileged command is executed.
        shim = """#!/usr/bin/env python3
import json, os, subprocess, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['INSTALL_REVIEW_LOG'], 'a') as log:
    log.write(json.dumps([name] + args) + '\\n')
if name == 'cmake' and args and args[0] == '-S':
    args[1] = os.environ['INSTALL_REVIEW_PROJECT']
    sys.exit(subprocess.call([os.environ['INSTALL_REVIEW_CMAKE']] + args))
"""
        for name in ("cmake", "sudo"):
            executable = binary / name
            executable.write_text(shim)
            executable.chmod(0o755)
        self.env = os.environ.copy()
        for name in ("SOAPYSIDEKIQ_INSTALL_PREFIX", "SIDEKIQ_SDK_DIR"):
            self.env.pop(name, None)
        self.env.update({
            "PATH": str(binary) + os.pathsep + self.env["PATH"],
            "SOAPYSIDEKIQ_BUILD_DIR": str(self.build),
            "INSTALL_REVIEW_LOG": str(self.log),
            "INSTALL_REVIEW_PROJECT": str(project),
            "INSTALL_REVIEW_CMAKE": shutil.which("cmake"),
        })

    def run_install(self, argument=None, prefix=None):
        env = self.env.copy()
        if prefix is not None:
            env["SOAPYSIDEKIQ_INSTALL_PREFIX"] = str(prefix)
        command = ["bash", str(REPO / "install.sh")]
        if argument is not None:
            command.append(argument)
        return subprocess.run(command, env=env, text=True, capture_output=True)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def effective_prefix(self):
        for line in (self.build / "CMakeCache.txt").read_text().splitlines():
            if line.startswith("CMAKE_INSTALL_PREFIX:"):
                return line.split("=", 1)[1]
        self.fail("No configured install prefix")

    def assert_unprivileged_install(self):
        self.assertFalse(any(command[0] == "sudo" for command in self.commands()))
        self.assertIn(["cmake", "--install", str(self.build)], self.commands())

    def test_positional_prefix_with_spaces(self):
        result = self.run_install(f"CMAKE_INSTALL_PREFIX={self.prefix}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.effective_prefix(), str(self.prefix))
        self.assert_unprivileged_install()

    def test_typed_positional_prefix(self):
        result = self.run_install(f"CMAKE_INSTALL_PREFIX:PATH={self.prefix}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.effective_prefix(), str(self.prefix))
        self.assert_unprivileged_install()

    def test_environment_prefix(self):
        result = self.run_install(prefix=self.prefix)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.effective_prefix(), str(self.prefix))
        self.assert_unprivileged_install()

    def test_explicit_prefix_overrides_environment(self):
        result = self.run_install(f"CMAKE_INSTALL_PREFIX={self.prefix}",
                                  prefix=self.root / "environment")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.effective_prefix(), str(self.prefix))
        self.assert_unprivileged_install()

    def test_default_run_clears_cached_isolated_prefix(self):
        self.assertEqual(self.run_install(prefix=self.prefix).returncode, 0)
        self.log.write_text("")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(self.effective_prefix(), str(self.prefix))
        self.assertIn(["sudo", "cmake", "--install", str(self.build)], self.commands())
        self.assertIn(["sudo", "ldconfig"], self.commands())

    def test_nonprefix_argument_keeps_default_install_mode(self):
        result = self.run_install("CMAKE_BUILD_TYPE=Debug")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CMAKE_BUILD_TYPE:UNINITIALIZED=Debug",
                      (self.build / "CMakeCache.txt").read_text())
        self.assertIn(["sudo", "cmake", "--install", str(self.build)], self.commands())

    def test_empty_prefix_is_rejected_before_any_command(self):
        result = self.run_install("CMAKE_INSTALL_PREFIX=")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.log.exists())
