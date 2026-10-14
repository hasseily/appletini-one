#!/usr/bin/env python3
"""Check fresh CPU1 builds and retained hardware archive provenance."""

import hashlib
import json
import struct
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path

import build_multiboot_arm as builder


class GlyphBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="multiboot-glyph-build-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def make_source_plan(self):
        names = ["amp.c", "lodepng.c", "psdma.c", "golden_self_update.c",
                 "multiboot.c", "multiboot_platform.c", "uart.c", "crc32.c"]
        for name in names:
            path = self.root / "ps_sources/lib" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* test source */\n")
        for name in builder.CORE1_SHARED_SOURCES:
            path = self.root / "ps_sources/frontend" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* test source */\n")
        main = self.root / "ps_sources/frontend_core1/main.c"
        main.parent.mkdir(parents=True)
        main.write_text("int main(void) { return 0; }\n")
        (self.root / "ps_sources/frontend/glyph_api.c").write_text("/* CPU0 only */\n")
        return builder.core1_source_plan(self.root)

    def make_config(self):
        config = self.root / "UserConfig.cmake"
        keys = ("USER_COMPILE_SOURCES", "USER_INCLUDE_DIRECTORIES",
                "USER_COMPILE_OPTIMIZATION_LEVEL", "USER_COMPILE_OTHER_FLAGS",
                "USER_LINK_LIBRARIES")
        config.write_text("\n".join(f"set({key}\n)" for key in keys))
        linker = self.root / "lscript.ld"
        linker.write_text("MEMORY {\nps7_ddr_0_memory_0 : ORIGIN = 0x100000, LENGTH = 0x3FF00000\n}\n")
        return config, linker

    def make_elf(self, *, entry=0x20000000, address=0x20000000,
                 vaddr=None, memory_bytes=0x1000, machine=40):
        data = bytearray(84)
        data[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HHI", data, 16, 2, machine, 1)
        struct.pack_into("<II", data, 24, entry, 52)
        struct.pack_into("<HHH", data, 40, 52, 32, 1)
        struct.pack_into("<8I", data, 52, 1, 0, address if vaddr is None else vaddr,
                         address, 0, memory_bytes, 7, 4)
        path = self.root / "core1.elf"
        path.write_bytes(data)
        return path

    def make_archive(self):
        payloads = {"hdl/top.sv": b"module top; endmodule\n",
                    "scripts/build.tcl": b"synth_design\n",
                    "third_party/lib/example.c": b"int example;\n",
                    "LICENSE": b"Test license\n"}
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()}
        # CPU sources and binary build outputs do not belong to the retained
        # hardware source archive, even when the baseline recorded them.
        hashes["ps_sources/frontend/main.c"] = "a" * 64
        hashes["hdl/generated.bin"] = "b" * 64
        records = {name: {"bytes": len(data), "sha256": hashes[name]}
                   for name, data in payloads.items()}
        archive = self.root / "hardware-sources.zip"
        manifest = self.root / "hardware-source-manifest.json"
        self.write_archive(archive, payloads)
        manifest.write_text(json.dumps({"files": records}))
        license_path = self.root / "LICENSE"
        license_path.write_bytes(payloads["LICENSE"])
        return archive, manifest, hashes, license_path, payloads

    @staticmethod
    def write_archive(path, payloads):
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in payloads.items():
                archive.writestr(name, data)

    def test_rebuild_is_opt_in_and_rejects_typo(self):
        self.assertFalse(builder.rebuild_core1_requested({}))
        self.assertTrue(builder.rebuild_core1_requested({"APP_MULTIBOOT_REBUILD_CORE1": "1"}))
        with self.assertRaisesRegex(RuntimeError, "must be 0 or 1"):
            builder.rebuild_core1_requested({"APP_MULTIBOOT_REBUILD_CORE1": "true"})

    def test_cpu1_plan_includes_renderer_and_excludes_cpu0_code(self):
        plan = self.make_source_plan()
        names = {path.name for path in plan["sources"]}
        self.assertTrue({"main.c", "uart.c", "crc32.c", "apple_cycle_renderer.c",
                         "apple_fb_handoff.c"}.issubset(names))
        self.assertFalse(builder.CORE1_EXCLUDED_LIBS & names)
        self.assertNotIn("glyph_api.c", names)
        self.assertEqual(plan["domain"], "standalone_ps7_cortexa9_1")

    def test_missing_shared_renderer_source_stops_build(self):
        self.make_source_plan()
        (self.root / "ps_sources/frontend/apple_fb_handoff.c").unlink()
        with self.assertRaises(FileNotFoundError):
            builder.core1_source_plan(self.root)

    def test_core1_configuration_uses_neon_o3_libm_and_upper_ddr(self):
        plan = self.make_source_plan()
        config, linker = self.make_config()
        builder.configure_core1(config, linker, plan)
        text = config.read_text()
        self.assertIn('set(USER_COMPILE_OPTIMIZATION_LEVEL\n"-O3"\n)', text)
        self.assertIn('set(USER_COMPILE_OTHER_FLAGS\n"-mfpu=neon"\n)', text)
        self.assertIn('set(USER_LINK_LIBRARIES\n"m"\n)', text)
        for path in plan["sources"] + plan["include_dirs"]:
            self.assertIn(f'"{path.as_posix()}"', text)
        self.assertIn("ORIGIN = 0x20000000, LENGTH = 0x10000000", linker.read_text())

    def test_ambiguous_or_missing_cpu1_ddr_region_is_rejected(self):
        plan = self.make_source_plan()
        config, linker = self.make_config()
        original = linker.read_text()
        for text in ("MEMORY {}", original + original):
            with self.subTest(text=text):
                linker.write_text(text)
                with self.assertRaisesRegex(RuntimeError, "expected one CPU1 DDR"):
                    builder.configure_core1(config, linker, plan)

    def test_cpu1_platform_requires_amp_bsp(self):
        platform = self.root / "platform.xpfm"
        bsp = self.root / "sw/standalone_ps7_cortexa9_1/bsp.yaml"
        with self.assertRaisesRegex(RuntimeError, "no CPU1 AMP domain"):
            builder.validate_core1_platform(platform)
        bsp.parent.mkdir(parents=True)
        bsp.write_text("proc_extra_compiler_flags:\n  name: proc_extra_compiler_flags\n  value: -DUSE_AMP=1\n")
        self.assertEqual(builder.validate_core1_platform(platform), bsp)
        bsp.write_text("proc_extra_compiler_flags:\n  value: ''\n")
        with self.assertRaisesRegex(RuntimeError, "must enable"):
            builder.validate_core1_platform(platform)

    def test_linked_core1_layout_is_checked(self):
        result = builder.audit_core1_layout(self.make_elf())
        self.assertEqual(result["entry"], 0x20000000)
        self.assertEqual(result["load_segments"][0]["address"], 0x20000000)
        for kwargs in ({"entry": 0x100000}, {"address": 0x100000},
                       {"address": 0x20001000}, {"memory_bytes": 0x10000001},
                       {"vaddr": 0x21000000}, {"machine": 3}):
            with self.subTest(kwargs=kwargs), self.assertRaises(RuntimeError):
                builder.audit_core1_layout(self.make_elf(**kwargs))

    def test_truncated_core1_elf_is_rejected(self):
        path = self.make_elf()
        path.write_bytes(path.read_bytes()[:-4])
        with self.assertRaisesRegex(RuntimeError, "truncated"):
            builder.audit_core1_layout(path)

    def test_archive_selection_requires_both_existing_files(self):
        self.assertIsNone(builder.hardware_archive_inputs({}))
        with self.assertRaisesRegex(RuntimeError, "select both"):
            builder.hardware_archive_inputs({"APP_MULTIBOOT_HARDWARE_ARCHIVE": "selected.zip"})
        with self.assertRaises(FileNotFoundError):
            builder.hardware_archive_inputs({"APP_MULTIBOOT_HARDWARE_ARCHIVE": "missing.zip",
                                             "APP_MULTIBOOT_HARDWARE_MANIFEST": "missing.json"})

    def test_valid_archive_matches_complete_baseline(self):
        archive, manifest, hashes, license_path, _ = self.make_archive()
        records = builder.validate_hardware_archive(archive, manifest, hashes, license_path)
        self.assertEqual(len(records), 4)
        self.assertEqual(builder.hardware_archive_inputs({"APP_MULTIBOOT_HARDWARE_ARCHIVE": str(archive),
                                                          "APP_MULTIBOOT_HARDWARE_MANIFEST": str(manifest)}),
                         (archive.resolve(), manifest.resolve()))

    def test_archive_payload_tamper_is_rejected_even_with_matching_new_manifest(self):
        archive, manifest, hashes, license_path, payloads = self.make_archive()
        payloads["hdl/top.sv"] = b"module changed; endmodule\n"
        self.write_archive(archive, payloads)
        records = json.loads(manifest.read_text())
        records["files"]["hdl/top.sv"] = {"bytes": len(payloads["hdl/top.sv"]),
                                           "sha256": hashlib.sha256(payloads["hdl/top.sv"]).hexdigest()}
        manifest.write_text(json.dumps(records))
        with self.assertRaisesRegex(RuntimeError, "member changed: hdl/top.sv"):
            builder.validate_hardware_archive(archive, manifest, hashes, license_path)

    def test_archive_manifest_hash_or_size_tamper_is_rejected(self):
        archive, manifest, hashes, license_path, _ = self.make_archive()
        original = manifest.read_text()
        for key, value in (("sha256", "0" * 64), ("bytes", 999)):
            records = json.loads(original)
            records["files"]["scripts/build.tcl"][key] = value
            manifest.write_text(json.dumps(records))
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "member changed"):
                builder.validate_hardware_archive(archive, manifest, hashes, license_path)

    def test_archive_missing_extra_or_duplicate_members_are_rejected(self):
        archive, manifest, hashes, license_path, payloads = self.make_archive()
        missing = dict(payloads)
        missing.pop("hdl/top.sv")
        extra = dict(payloads, **{"scripts/unrecorded.py": b"pass\n"})
        for contents in (missing, extra):
            self.write_archive(archive, contents)
            with self.subTest(contents=contents), self.assertRaisesRegex(RuntimeError, "members"):
                builder.validate_hardware_archive(archive, manifest, hashes, license_path)
        self.write_archive(archive, payloads)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(archive, "a") as stream:
                stream.writestr("hdl/top.sv", payloads["hdl/top.sv"])
        with self.assertRaisesRegex(RuntimeError, "members"):
            builder.validate_hardware_archive(archive, manifest, hashes, license_path)

    def test_manifest_cannot_drop_a_baseline_file(self):
        archive, manifest, hashes, license_path, _ = self.make_archive()
        records = json.loads(manifest.read_text())
        records["files"].pop("hdl/top.sv")
        manifest.write_text(json.dumps(records))
        with self.assertRaisesRegex(RuntimeError, "complete baseline"):
            builder.validate_hardware_archive(archive, manifest, hashes, license_path)


if __name__ == "__main__":
    unittest.main()
