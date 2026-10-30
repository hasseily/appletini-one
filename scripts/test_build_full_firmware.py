"""Exercise full-build input gates without invoking Vivado, Vitis or GCC."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import build_full_firmware as build
from package_timing_firmware import write_manifest

ROOT = Path(__file__).resolve().parents[1]


class FullFirmwareInputTests(unittest.TestCase):
    def setUp(self):
        folder = ROOT / "build/f136-packaging-preflight/temp"
        folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.override = patch.object(build, "ROOT", self.root)
        self.override.start()
        self.addCleanup(self.override.stop)
        self.run_dir = self.root / ".timing_runs/20261030T164234Z-a8624920-full"
        self.run_dir.mkdir(parents=True)
        (self.run_dir / "appletini_yarz_top.bit").write_bytes(b"named-new-FPGA")
        (self.run_dir / "candidate.dcp").write_bytes(b"named-new-DCP")
        with zipfile.ZipFile(self.run_dir / "appletini_yarz_top.xsa", "w") as archive:
            archive.writestr("hw/appletini_yarz_top.bit", b"named-new-FPGA")
        self.manifest = dict(build_id=self.run_dir.name, git_dirty="1", git_sha="a" * 40, status="exported",
            build_mode="full", synthesis_reused="0", rescue_used="0", incremental_reference="",
            route_status="PASS", bus_skew_status="PASS", constraint_bounds_status="PASS",
            minimum_wns_ns="0.050", wns_ns="0.075", whs_ns="0.010", wpws_ns="0.100",
            implementation_setup_margin_ns="0.200", final_direction_a_limit_ns="10",
            final_direction_d_limit_ns="10", final_phi0_release_limit_ns="8")
        for name in ("tns_ns", "ths_ns", "tpws_ns", "setup_failing_endpoints", "hold_failing_endpoints",
                     "pulse_width_failing_endpoints", "unconstrained_internal_endpoints", "route_errors",
                     "missing_constraint_objects", "final_fabric_user_uncertainty_ns", "final_pixel_user_uncertainty_ns"):
            self.manifest[name] = "0"
        for key, filename in (("bitstream_sha256", build.PL_FILES[0]), ("xsa_sha256", build.PL_FILES[1]), ("candidate_dcp_sha256", build.PL_FILES[2])):
            self.manifest[key] = build.sha(self.run_dir / filename)
        self.save()

    def save(self):
        write_manifest(self.run_dir / "manifest.txt", self.manifest)

    def test_dirty_manifest_accepted_without_rewriting_it(self):
        original = (self.run_dir / "manifest.txt").read_bytes()
        self.assertEqual(build.validate_run(self.run_dir)["git_dirty"], "1")
        self.assertEqual((self.run_dir / "manifest.txt").read_bytes(), original)

    def test_timing_route_reuse_and_identity_fail_closed(self):
        for key, value in (("wns_ns", "0.049"), ("minimum_wns_ns", "0"), ("whs_ns", "-0.001"),
                           ("wns_ns", "nan"), ("route_status", "FAIL"), ("synthesis_reused", "1"),
                           ("rescue_used", "1"), ("incremental_reference", "old.dcp"),
                           ("git_dirty", "unknown"), ("build_id", "different-run")):
            with self.subTest(key=key, value=value):
                old = self.manifest[key]
                self.manifest[key] = value
                self.save()
                with self.assertRaises((RuntimeError, ValueError, AssertionError)): build.validate_run(self.run_dir)
                self.manifest[key] = old
        self.save()

    def test_each_named_artifact_hash_is_checked(self):
        for name in build.PL_FILES:
            with self.subTest(name=name):
                path = self.run_dir / name
                previous = path.read_bytes()
                path.write_bytes(previous + b"changed")
                with self.assertRaises((RuntimeError, ValueError, AssertionError)): build.validate_run(self.run_dir)
                path.write_bytes(previous)

    def test_xsa_embedded_bit_checked_even_with_valid_outer_hash(self):
        with zipfile.ZipFile(self.run_dir / build.PL_FILES[1], "w") as archive:
            archive.writestr("wrong.bit", b"old-FPGA")
        self.manifest["xsa_sha256"] = build.sha(self.run_dir / build.PL_FILES[1])
        self.save()
        with self.assertRaises(RuntimeError): build.validate_run(self.run_dir)

    def test_audit_uses_only_copied_artifact_inputs(self):
        out = self.root / "build/candidate"
        (out / "inputs").mkdir(parents=True)
        for name in ("platform-source.xsa", "platform-export.xsa"):
            (out / "inputs" / name).write_bytes(b"copied-platform")
        command = build.audit_command(out, "F1.3.6", "B2.0.6")
        self.assertEqual(command.count("--platform-xsa"), 2)
        self.assertIn(str(out / "inputs/candidate.dcp"), command)
        self.assertNotIn("vitis_workspace", " ".join(command))
        rerun = out / "separate-audit.json"
        self.assertEqual(build.audit_command(out, "F1.3.6", "B2.0.6", rerun)[-5], str(rerun))

    def test_source_archive_excludes_real_and_legacy_seeds_and_build_metadata(self):
        keep = ("ps_sources/example.c", "scripts/fixture.pem", "README.md")
        reject = ("software/https_setup/ATNETA.SED", "software/https_setup/atnetb.sed",
                  "scripts/unknown.SED", "scripts/ATNSEEDA.BIN", "scripts/ATNSEEDB.BIN",
                  "scripts/build/generated.c", "third_party/.git/config", "ps_sources/unwanted.o")
        for name in (*keep, *reject):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        members = build.source_members({"sources": {keep[0]: "hash"}, "fpga_source_hashes": {reject[0]: "hash"}})
        self.assertTrue(set(keep).issubset(members))
        self.assertFalse(set(reject).intersection(members))

    def test_cli_exit_zero_without_completion_receipt_stops_before_output_copy(self):
        out = self.root / "build/candidate"
        out.mkdir(parents=True)
        header = self.root / "ps_sources/image_versions.h"
        header.parent.mkdir()
        header.write_text('"F1.3.6" "B2.0.6"\n')
        (self.root / "project").mkdir()
        boot = self.root / "BOOT.BIN"
        boot.write_bytes(b"unchanged-Golden-fixture")
        tools = ("build_full_firmware.py", "vitis_app_build.py", "create_vitis_workspace.py", "http_build_config.py",
                 "frontend_memory.py", "make_firmware_bin.bat", "audit_firmware_release.py", "image_manifest.py", "package_timing_firmware.py")
        (self.root / "scripts").mkdir()
        for name in tools: (self.root / "scripts" / name).write_text("test-only build tool")
        (out / "SOURCE_FREEZE.json").write_text(json.dumps(dict(git_head="a" * 40, firmware_version="F1.3.6", boot_version="B2.0.6",
            arm_source_hashes=build.arm_sources(), fpga_source_hashes={})))
        argv = ["build_full_firmware.py", "--version", "F1.3.6", "--timing-run", str(self.run_dir), "--output", str(out), "--build-only"]
        with patch.object(sys, "argv", argv), patch.object(build, "BOOT_SHA", build.sha(boot)), \
             patch.object(build.subprocess, "check_output", return_value="a" * 40 + "\n"), patch.object(build, "run", return_value=None) as cli:
            with self.assertRaisesRegex(RuntimeError, "without a successful workspace completion receipt"): build.main()
            self.assertEqual(cli.call_count, 1)
        self.assertFalse((out / "FIRMWARE.BIN").exists())
        self.assertFalse((out / "inputs/fsbl.elf").exists())


if __name__ == "__main__":
    unittest.main()
