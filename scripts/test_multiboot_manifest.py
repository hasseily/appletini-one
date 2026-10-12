"""Check product identity, legacy compatibility and safe firmware packaging."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import image_manifest as image


class MultibootManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.raw = self.path / "raw.bin"
        self.output = self.path / "FIRMWARE.BIN"

    def payload(self, product=None):
        data = b"payload" * 127 + image.RECOVERY_MARKER
        if product:
            data += image.PRODUCT_MARKER[image.PRODUCT_BY_NAME[product]]
        self.raw.write_bytes(data)
        return data

    def package(self, product):
        self.payload(product)
        return image.append_manifest(self.raw, self.output,
                                     image.ROLE_FIRMWARE, product=product)

    def test_v2_exact_product_flags_and_slot_routing(self):
        for product, expected in (("appletini", 0x501), ("multitini", 0x601)):
            manifest = self.package(product)
            self.assertEqual((manifest.version, manifest.role, manifest.flags), (2, 2, expected))
            self.assertEqual(image.verify_image(self.output, expected_product=product), manifest)
            result = subprocess.run([sys.executable, str(ROOT / "scripts/image_manifest.py"),
                                     "slot", str(self.output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            p = image.PRODUCT_BY_NAME[product]
            self.assertEqual(result.stdout.strip(), f"0x{image.PRODUCT_OFFSET[p]:08X} 0x{image.PRODUCT_CAPACITY[p]:08X}")

    def test_old_firmware_cannot_be_retagged_without_linked_family(self):
        self.payload()
        self.output.write_bytes(b"previous destination")
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE, product="multitini")
        self.assertEqual(self.output.read_bytes(), b"previous destination")

    def test_wrong_family_and_both_families_are_rejected(self):
        self.payload("appletini")
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE, product="multitini")
        self.raw.write_bytes(self.raw.read_bytes() + image.PRODUCT_MARKER[image.PRODUCT_MULTITINI])
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE, product="appletini")

    def test_recovery_marker_is_required(self):
        self.raw.write_bytes(image.PRODUCT_MARKER[image.PRODUCT_APPLETINI])
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE, product="appletini")

    def test_v1_firmware_remains_verifiable_but_has_no_slot_identity(self):
        self.payload()
        old = image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE,
                                    image.FLAG_RECOVERY_CAPABLE)
        self.assertEqual(old.version, 1)
        self.assertEqual(image.verify_image(self.output), old)
        with self.assertRaises(image.ManifestError):
            image.verify_image(self.output, expected_product="appletini")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/image_manifest.py"),
                                 "slot", str(self.output)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_golden_stays_generic_v1(self):
        self.payload()
        golden = image.append_manifest(self.raw, self.output, image.ROLE_GOLDEN,
                                       image.FLAG_RECOVERY_CAPABLE)
        self.assertEqual((golden.version, golden.flags), (1, 1))
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_GOLDEN, product="appletini")

    def test_v2_flag_contract_rejects_missing_or_ambiguous_bits(self):
        for flags in (0, 1, 0x101, 0x401, 0x701, 0x501 | 0x800):
            with self.assertRaises(image.ManifestError):
                image.encode_manifest(image.ROLE_FIRMWARE, flags, 100, 0, version=2)
        with self.assertRaises(image.ManifestError):
            image.encode_manifest(image.ROLE_GOLDEN, 0x501, 100, 0, version=2)

    def test_product_slot_capacity_is_enforced_without_explicit_maximum(self):
        self.payload("appletini")
        with self.raw.open("ab") as out:
            out.truncate(image.PRODUCT_CAPACITY[image.PRODUCT_APPLETINI])
        with self.assertRaises(image.ManifestError):
            image.append_manifest(self.raw, self.output, image.ROLE_FIRMWARE, product="appletini")


if __name__ == "__main__":
    unittest.main()
