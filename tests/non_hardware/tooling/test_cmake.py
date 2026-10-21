"""Exercise SDK selection/link usage and staged uninstall with real CMake."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[3]


class CMakeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cmake probe ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.build = self.root / "build"
        self.compiler = os.environ.get("SOAPYSIDEKIQ_TEST_CXX", shutil.which("c++"))
        self.env = os.environ.copy()
        self.env.pop("SIDEKIQ_SDK_DIR", None)
        self.env.pop("SIDEKIQ_CONFIG_BUILD_CONFIG", None)
        self.env.pop("DESTDIR", None)
        (self.source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.21)
project(SdkProbe LANGUAGES CXX)
list(PREPEND CMAKE_MODULE_PATH "{REPO}")
find_package(Sidekiq 4.26 REQUIRED)
file(WRITE "${{CMAKE_BINARY_DIR}}/sdk-build-config.txt" "${{Sidekiq_BUILD_CONFIG}}")
add_executable(probe main.cpp)
target_compile_definitions(probe PRIVATE EXPECTED_MARKER=${{EXPECTED_MARKER}})
target_link_libraries(probe PRIVATE Sidekiq::sidekiq)
''')
        (self.source / "main.cpp").write_text('''#include <fixture.hpp>
static_assert(SDK_MARKER == EXPECTED_MARKER, "SDK compile flags were not propagated");
int main() { return sdk_answer() == EXPECTED_MARKER ? 0 : 1; }
''')

    def command(self, *args, env=None):
        return subprocess.run(args, env=self.env if env is None else env,
                              capture_output=True, text=True)

    def make_sdk(self, name="SDK with spaces", marker=17, version="4.26.0"):
        if not sys.platform.startswith("linux"):
            self.skipTest("SDK archive fixture uses Linux linker group flags")
        sdk = self.root / name
        for directory in ("bin", "inc", "lib"):
            (sdk / directory).mkdir(parents=True)
        (sdk / "inc/fixture.hpp").write_text("int sdk_answer();\n")
        # A cyclic archive dependency genuinely requires the group to surround
        # both libraries; hoisting group flags ahead of libraries would fail.
        (sdk / "sdk.cpp").write_text("int support_answer(); int sdk_answer() { return support_answer(); }\n")
        (sdk / "detail.cpp").write_text(f"int sdk_detail() {{ return {marker}; }}\n")
        (sdk / "support.cpp").write_text("int sdk_detail(); int support_answer() { return sdk_detail(); }\n")
        for stem in ("sdk", "detail", "support"):
            result = self.command(self.compiler, "-c", str(sdk / f"{stem}.cpp"),
                                  "-o", str(sdk / f"{stem}.o"))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.command("ar", "rcs", str(sdk / "lib/libfixture.a"),
                              str(sdk / "sdk.o"), str(sdk / "detail.o"))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command("ar", "rcs", str(sdk / "lib/libsupport.a"), str(sdk / "support.o"))
        self.assertEqual(result.returncode, 0, result.stderr)
        flags = ["-I", str(sdk / "inc"), f"-DSDK_MARKER={marker}"]
        libs = ["-Wl,--start-group", str(sdk / "lib/libfixture.a"),
                str(sdk / "lib/libsupport.a"), "-Wl,--end-group"]
        responses = {
            "--prefix": str(sdk), "--version": version,
            "--support-dir": str(sdk / "lib"), "--build-config": "x86_64.gcc",
            "--cflags": shlex.join(flags), "--libs-static": shlex.join(libs),
        }
        script = sdk / "bin/sidekiq-config"
        script.write_text("#!/usr/bin/env python3\nimport os, sys\n"
                          f"responses = {responses!r}\n"
                          "if sys.argv[1] == '--build-config':\n"
                          "    print(os.environ.get('SIDEKIQ_CONFIG_BUILD_CONFIG', 'x86_64.gcc'))\n"
                          "else:\n    print(responses[sys.argv[1]])\n")
        script.chmod(0o755)
        return sdk

    def configure(self, sdk=None, marker=17, env=None):
        args = ["cmake", "-S", str(self.source), "-B", str(self.build),
                f"-DCMAKE_CXX_COMPILER={self.compiler}", f"-DEXPECTED_MARKER={marker}"]
        if sdk is not None:
            args.append(f"-DSIDEKIQ_SDK_DIR={sdk}")
        return self.command(*args, env=env)

    def build_and_run(self):
        result = self.command("cmake", "--build", str(self.build), "--parallel", "2")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.command(str(self.build / "probe"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_imported_target_propagates_quoted_flags_and_ordered_static_link(self):
        result = self.configure(self.make_sdk())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.build_and_run()

    def test_reconfigure_switches_sdk_without_clearing_cache(self):
        self.assertEqual(self.configure(self.make_sdk("first")).returncode, 0)
        result = self.configure(self.make_sdk("second", marker=23), marker=23)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.build_and_run()

    def test_missing_sdk_does_not_reuse_cached_executable(self):
        self.assertEqual(self.configure(self.make_sdk()).returncode, 0)
        result = self.configure(self.root / "missing")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could NOT find Sidekiq", result.stdout + result.stderr)

    def test_old_sdk_is_rejected(self):
        result = self.configure(self.make_sdk(version="4.25.0"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("4.25.0", result.stdout + result.stderr)

    def test_failed_config_command_reports_operation(self):
        sdk = self.make_sdk()
        (sdk / "bin/sidekiq-config").write_text("#!/bin/sh\necho 'fixture failure' >&2\nexit 2\n")
        result = self.configure(sdk)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--cflags failed", " ".join((result.stdout + result.stderr).split()))
        self.assertIn("fixture failure", result.stdout + result.stderr)

    def test_environment_selects_sdk(self):
        env = dict(self.env, SIDEKIQ_SDK_DIR=str(self.make_sdk()))
        result = self.configure(env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.build_and_run()

    def test_explicit_sdk_overrides_environment(self):
        env = dict(self.env, SIDEKIQ_SDK_DIR=str(self.root / "missing"))
        result = self.configure(self.make_sdk(), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.build_and_run()

    def test_arm_build_config_is_reported_without_host_path_assumptions(self):
        # Metadata behavior only; this is not an ARM cross-compilation test.
        env = dict(self.env, SIDEKIQ_CONFIG_BUILD_CONFIG="aarch64")
        result = self.configure(self.make_sdk(), env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.build / "sdk-build-config.txt").read_text(), "aarch64")

    def test_incomplete_include_flag_is_rejected(self):
        sdk = self.make_sdk()
        script = sdk / "bin/sidekiq-config"
        script.write_text(script.read_text().replace("print(responses[sys.argv[1]])",
                          "print('-I' if sys.argv[1] == '--cflags' else responses[sys.argv[1]])"))
        result = self.configure(sdk)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("without a directory", result.stdout + result.stderr)

    def install_project(self, prefix):
        (self.source / "payload").write_text("first payload\n")
        (self.source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.21)
project(InstallProbe NONE)
install(FILES payload DESTINATION share/probe)
add_custom_target(uninstall COMMAND "${{CMAKE_COMMAND}}"
    "-DMANIFEST=${{CMAKE_BINARY_DIR}}/install_manifest.txt"
    -P "{REPO}/cmake/Uninstall.cmake" VERBATIM)
''')
        result = self.command("cmake", "-S", str(self.source), "-B", str(self.build),
                              f"-DCMAKE_INSTALL_PREFIX={prefix}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sdk_config_is_a_host_tool_even_with_target_root_search(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace(
            "find_package(Sidekiq 4.26 REQUIRED)",
            'set(CMAKE_FIND_ROOT_PATH "/nonexistent-target-root")\n'
            'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)\n'
            'find_package(Sidekiq 4.26 REQUIRED)',
        ))
        result = self.configure(self.make_sdk())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.build_and_run()

    def test_overwrite_and_uninstall_preserve_unrelated_files(self):
        prefix = self.root / "isolated install"
        self.install_project(prefix)
        result = self.command("cmake", "--install", str(self.build))
        self.assertEqual(result.returncode, 0, result.stderr)
        (prefix / "unrelated").write_text("keep")
        payload = self.source / "payload"
        previous = payload.stat()
        payload.write_text("replacement payload of a different size\n")
        # CMake's install copy compares timestamps; ensure a distinct timestamp
        # without a sleep or reliance on the filesystem's timestamp precision.
        os.utime(payload, (previous.st_atime, previous.st_mtime + 5))
        self.assertEqual(self.command("cmake", "--install", str(self.build)).returncode, 0)
        self.assertEqual((prefix / "share/probe/payload").read_text(),
                         "replacement payload of a different size\n")
        for _ in range(2):
            result = self.command("cmake", "--build", str(self.build), "--target", "uninstall")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((prefix / "share/probe/payload").exists())
        self.assertEqual((prefix / "unrelated").read_text(), "keep")

    def test_uninstall_honors_destdir(self):
        self.install_project("/usr")
        stage = self.root / "stage"
        env = dict(self.env, DESTDIR=str(stage))
        self.assertEqual(self.command("cmake", "--install", str(self.build), env=env).returncode, 0)
        installed = stage / "usr/share/probe/payload"
        self.assertTrue(installed.exists())
        result = self.command("cmake", "--build", str(self.build), "--target", "uninstall", env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(installed.exists())

    def test_soapy_module_install_respects_prefix_relocation_and_destdir(self):
        # Reproduce the old helper's absolute dependency-root default and the
        # newer relative/ABI-qualified default. The production adapter must
        # preserve both --prefix relocation and staging without touching the
        # dependency installation. No SDK or compiler is needed for this rule.
        for version, legacy in (("0.4.0", True), ("0.4.1", True),
                                ("0.4.2", False), ("0.8.1", False)):
            with self.subTest(version=version):
                source = self.root / version
                source.mkdir()
                module_directory = "modules0.8-3" if version == "0.8.1" else "modules"
                dependency = source / "dependency"
                dependency.mkdir()
                (dependency / "unrelated").write_text("keep")
                (source / "payload").write_text("module fixture")
                (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.21)
project(ModuleInstallProbe NONE)
set(SoapySDR_VERSION "{version}")
set(SOAPY_SDR_ROOT "{dependency}")
set(LEGACY_HELPER {"TRUE" if legacy else "FALSE"})
function(SOAPY_SDR_MODULE_UTIL)
    cmake_parse_arguments(MODULE "" "PREFIX" "" ${{ARGN}})
    if(LEGACY_HELPER)
        if(NOT MODULE_PREFIX)
            set(MODULE_PREFIX "${{SOAPY_SDR_ROOT}}")
        endif()
        set(destination "${{MODULE_PREFIX}}/lib/SoapySDR/modules")
    else()
        set(destination "lib/SoapySDR/{module_directory}")
        if(MODULE_PREFIX)
            set(destination "${{MODULE_PREFIX}}/${{destination}}")
        endif()
    endif()
    install(FILES payload DESTINATION "${{destination}}")
endfunction()
include("{REPO}/cmake/SoapyModule.cmake")
soapy_sidekiq_add_module()
''')
                build = source / "build"
                configured_prefix = source / "configured prefix"
                result = self.command("cmake", "-S", str(source), "-B", str(build),
                                      f"-DCMAKE_INSTALL_PREFIX={configured_prefix}")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                relocated = source / "relocated prefix"
                result = self.command("cmake", "--install", str(build),
                                      "--prefix", str(relocated))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                suffix = Path("lib/SoapySDR") / module_directory / "payload"
                self.assertEqual((relocated / suffix).read_text(), "module fixture")
                self.assertFalse(configured_prefix.exists())
                self.assertEqual(list(dependency.iterdir()), [dependency / "unrelated"])
                env = self.env.copy()
                stage = source / "staged"
                env["DESTDIR"] = str(stage)
                result = self.command("cmake", "--install", str(build),
                                      "--prefix", "/opt/sidekiq fixture", env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((stage / "opt/sidekiq fixture" / suffix).read_text(),
                                 "module fixture")
                self.assertEqual((dependency / "unrelated").read_text(), "keep")

    def test_uninstall_requires_manifest(self):
        self.install_project(self.root / "prefix")
        result = self.command("cmake", "--build", str(self.build), "--target", "uninstall")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Install manifest not found", result.stdout + result.stderr)

    def test_uninstall_refuses_directory_entries(self):
        self.install_project(self.root / "prefix")
        (self.build / "install_manifest.txt").write_text(str(self.source) + "\n")
        result = self.command("cmake", "--build", str(self.build), "--target", "uninstall")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Refusing to remove a directory", result.stdout + result.stderr)
        self.assertTrue(self.source.is_dir())
