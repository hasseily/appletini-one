"""Native guard tests; no Vitis, ARM compiler, workspace deletion or hardware."""
from pathlib import Path
import json
import os
from subprocess import CompletedProcess
import tempfile
import unittest
from unittest.mock import patch

import vitis_app_build as build

ROOT = Path(__file__).resolve().parents[1]


class VitisAppBuildTests(unittest.TestCase):
    def setUp(self):
        folder = ROOT / "build/f136-packaging-preflight/temp"
        folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name)
        self.path = self.workspace / "frontend/build"
        (self.path / "CMakeFiles").mkdir(parents=True)
        (self.path / "CMakeCache.txt").write_text("CMAKE_COMMAND:INTERNAL=unused-cmake\n")
        (self.path / "build.ninja").write_text(
            "build frontend.elf: C_EXECUTABLE_LINKER__frontend object.obj\n"
            '  FLAGS = -O2 -I"C:/a path/include" -mfpu=neon\n'
            "  LINK_FLAGS = -Wl,--test-extra\n"
            f'  PRE_LINK = cd .\n  POST_BUILD = cmd.exe /C "cd /D {self.path.resolve()} && arm-none-eabi-size --format=berkeley frontend.elf '
            f'&& arm-none-eabi-size --format=berkeley frontend.elf > {self.path.resolve().as_posix()}/frontend.elf.size"\n'
            "  RSP_FILE = CMakeFiles\\frontend.elf.rsp\n  TARGET_FILE = frontend.elf\n\n")
        (self.path / "CMakeFiles/rules.ninja").write_text(
            'rule C_EXECUTABLE_LINKER__frontend\n  command = cmd.exe /C "$PRE_LINK && '
            'C:/tool/arm-none-eabi-gcc.exe $FLAGS -Wl,--defsym,_HEAP_SIZE=0x04000000,--defsym,_STACK_SIZE=0x00010000 '
            '$LINK_FLAGS @$RSP_FILE -o $TARGET_FILE && $POST_BUILD"\n\n')
        (self.path / "CMakeFiles/frontend.elf.rsp").write_text("object.obj -Llib -lxil\n")
        (self.path / "frontend.elf").write_bytes(b"\x7fELFtest")
        self.component = type("Component", (), {"build": lambda self: 0})()
        self.environment = patch.dict(os.environ, {"XILINX_VITIS": "C:/test-vitis"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_generated_flags_response_and_quoting_preserved(self):
        command = build.native_link_command(self.path, "frontend")
        self.assertEqual(command, ["C:/tool/arm-none-eabi-gcc.exe", "-O2", "-IC:/a path/include", "-mfpu=neon",
            "-Wl,--defsym,_HEAP_SIZE=0x04000000,--defsym,_STACK_SIZE=0x00010000", "-Wl,--test-extra",
            "@CMakeFiles/frontend.elf.rsp", "-o", "frontend.elf"])

    def test_response_file_required(self):
        (self.path / "CMakeFiles/frontend.elf.rsp").unlink()
        with self.assertRaises(RuntimeError): build.native_link_command(self.path, "frontend")

    def test_unrecognized_rule_and_nontrivial_prelink_rejected(self):
        path = self.path / "CMakeFiles/rules.ninja"
        original = path.read_text()
        path.write_text(original.replace("arm-none-eabi-gcc.exe", "different-linker.exe"))
        with self.assertRaises(RuntimeError): build.native_link_command(self.path, "frontend")
        path.write_text(original)
        path = self.path / "build.ninja"
        path.write_text(path.read_text().replace("PRE_LINK = cd .", "PRE_LINK = important-command"))
        with self.assertRaises(RuntimeError): build.native_link_command(self.path, "frontend")

    def test_only_known_elf_wrapper_failure_is_eligible(self):
        diagnostic = "The system cannot execute the specified program."
        self.assertTrue(build.known_link_wrapper_failure("FAILED: frontend.elf \n" + diagnostic, "frontend"))
        for log in ("FAILED: object.obj\n" + diagnostic, "FAILED: frontend.elf\nerror: undefined reference",
                    "FAILED: object.obj\nFAILED: frontend.elf\n" + diagnostic):
            self.assertFalse(build.known_link_wrapper_failure(log, "frontend"))

    def test_non_size_postlink_hook_is_never_discarded(self):
        path = self.path / "build.ninja"
        path.write_text(path.read_text().replace('POST_BUILD = cmd.exe', 'POST_BUILD = important-step && cmd.exe'))
        with self.assertRaises(RuntimeError): build.native_link_command(self.path, "frontend")

    def test_normal_build_never_uses_fallback(self):
        with patch.object(build.subprocess, "run", return_value=CompletedProcess([], 0, "normal build success\n", "")) as run:
            self.assertEqual(build.build_app_component(self.component, "frontend", self.workspace), 0)
            self.assertEqual(run.call_count, 1)
        self.assertFalse((self.path / "DIRECT_LINK_COMMAND.json").exists())

    def test_known_wrapper_fallback_links_then_sizes_exact_elf(self):
        results = [CompletedProcess([], 1, "FAILED: frontend.elf \nThe system cannot execute the specified program.\n", ""),
                   CompletedProcess([], 0, "linked\n", ""), CompletedProcess([], 0, "size verified\n", "")]
        with patch.object(build.subprocess, "run", side_effect=results) as run:
            self.assertEqual(build.build_app_component(self.component, "frontend", self.workspace), 0)
            self.assertEqual(run.call_args_list[1].args[0], build.native_link_command(self.path, "frontend"))
            self.assertTrue(run.call_args_list[2].args[0][0].endswith("arm-none-eabi-size.exe"))
        self.assertEqual(json.loads((self.path / "DIRECT_LINK_COMMAND.json").read_text()), build.native_link_command(self.path, "frontend"))

    def test_compile_error_and_native_link_error_are_fatal(self):
        with patch.object(build.subprocess, "run", return_value=CompletedProcess([], 1, "FAILED: object.obj\ncompiler error", "")) as run:
            with self.assertRaises(RuntimeError): build.build_app_component(self.component, "frontend", self.workspace)
            self.assertEqual(run.call_count, 1)
        results = [CompletedProcess([], 1, "FAILED: frontend.elf\nThe system cannot execute the specified program.", ""),
                   CompletedProcess([], 1, "", "undefined reference")]
        with patch.object(build.subprocess, "run", side_effect=results) as run:
            with self.assertRaises(RuntimeError): build.build_app_component(self.component, "frontend", self.workspace)
            self.assertEqual(run.call_count, 2)

    def test_workspace_script_never_kills_processes(self):
        source = (ROOT / "scripts/create_vitis_workspace.py").read_text()
        self.assertNotIn("taskkill", source)
        self.assertIn('workspace_path != expected_workspace', source)
        self.assertIn('build_app_component(comp, name)', source)
        self.assertIn('build_app_component(frontend_comp, "frontend")', source)


class BspRegenerationTests(unittest.TestCase):
    def setUp(self):
        folder = ROOT / "build/f136-packaging-preflight/temp"
        folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.addCleanup(self.temp.cleanup)
        self.bsp = Path(self.temp.name) / "bsp"
        self.complete()
        self.sleep = patch.object(build.time, "sleep")
        self.sleep_mock = self.sleep.start()
        self.addCleanup(self.sleep.stop)

    def complete(self):
        files = {"bsp.yaml": "generated metadata", "CMakeLists.txt": "set(BSP_LIBSRC_SUBDIRS libsrc standalone xiltimer)",
                 "cortexa9_toolchain.cmake": "toolchain", "Findcommon.cmake": "common", "Findcommonmeta.cmake": "metadata",
                 "libsrc/CMakeLists.txt": "add_subdirectory(${DRIVER}/src)", "libsrc/DRVLISTConfig.cmake": "set(DRIVER_LIST common;sdps)"}
        files.update({f"libsrc/{name}/src/CMakeLists.txt": "driver" for name in ("standalone", "xiltimer", "common", "sdps")})
        for name, text in files.items():
            path = self.bsp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def domain(self, fn):
        return type("Domain", (), {"regenerate": lambda unused: fn()})()

    def test_successful_none_return_is_supported(self):
        build.regenerate_bsp_checked(self.domain(lambda: None), self.bsp)
        self.sleep_mock.assert_not_called()

    def test_none_error_output_is_not_masked_by_complete_old_sources(self):
        calls = []
        def regenerate():
            calls.append(1)
            if len(calls) == 1: print("Traceback: transient file lock\nError in regenerating BSP")
        build.regenerate_bsp_checked(self.domain(regenerate), self.bsp)
        self.assertEqual(len(calls), 2)
        self.sleep_mock.assert_called_once_with(1.0)

    def test_partial_driver_tree_retries_then_requires_complete_sources(self):
        (self.bsp / "libsrc/sdps/src/CMakeLists.txt").unlink()
        calls = []
        def regenerate():
            calls.append(1)
            if len(calls) == 2: self.complete()
        build.regenerate_bsp_checked(self.domain(regenerate), self.bsp)
        self.assertEqual(len(calls), 2)
        build.validate_bsp_sources(self.bsp)

    def test_repeated_error_and_repeated_partial_tree_fail_closed(self):
        for error in (True, False):
            with self.subTest(error_output=error):
                self.complete()
                if not error: (self.bsp / "libsrc/common/src/CMakeLists.txt").unlink()
                calls = []
                def regenerate():
                    calls.append(1)
                    if error: print("[ERROR]: failed to generate BSP")
                with self.assertRaises(RuntimeError): build.regenerate_bsp_checked(self.domain(regenerate), self.bsp)
                self.assertEqual(len(calls), 2)

    def test_metadata_and_driver_graph_are_required(self):
        for name in ("bsp.yaml", "libsrc/DRVLISTConfig.cmake", "libsrc/standalone/src/CMakeLists.txt"):
            with self.subTest(name=name):
                self.complete()
                (self.bsp / name).unlink()
                with self.assertRaises(RuntimeError): build.validate_bsp_sources(self.bsp)


class WorkspaceReceiptTests(unittest.TestCase):
    def setUp(self):
        folder = ROOT / "build/f136-packaging-preflight/temp"
        folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name) / "vitis_workspace"
        for name, path in build.workspace_receipt_paths(self.workspace).items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"new-XSA" if name.endswith("xsa") else b"fresh-ELF-or-blob")
        self.token = patch.dict(os.environ, {"APPLETINI_VITIS_BUILD_TOKEN": "fresh-nonce"})
        self.token.start()
        self.addCleanup(self.token.stop)
        self.xsa = build.hashlib.sha256(b"new-XSA").hexdigest()

    def test_fresh_matching_receipt_and_hashes_pass(self):
        build.write_workspace_receipt(self.workspace)
        self.assertEqual(build.verify_workspace_receipt(self.workspace, self.xsa, "fresh-nonce")["status"], "PASS")
        with self.assertRaises(FileExistsError): build.write_workspace_receipt(self.workspace)

    def test_cli_success_without_receipt_and_stale_token_fail(self):
        with self.assertRaisesRegex(RuntimeError, "without a successful"): build.verify_workspace_receipt(self.workspace, self.xsa, "fresh-nonce")
        build.write_workspace_receipt(self.workspace)
        with self.assertRaisesRegex(RuntimeError, "stale"): build.verify_workspace_receipt(self.workspace, self.xsa, "another-build")

    def test_changed_elf_or_different_xsa_fails(self):
        build.write_workspace_receipt(self.workspace)
        with self.assertRaises(RuntimeError): build.verify_workspace_receipt(self.workspace, "0" * 64, "fresh-nonce")
        build.workspace_receipt_paths(self.workspace)["core1_blob"].write_bytes(b"different")
        with self.assertRaisesRegex(RuntimeError, "core1_blob"): build.verify_workspace_receipt(self.workspace, self.xsa, "fresh-nonce")


if __name__ == "__main__":
    unittest.main()
