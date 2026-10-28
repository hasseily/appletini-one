#!/usr/bin/env python3
"""Check HTTPS acceptance gates against recorded, redacted board transcripts."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

import https_probe_host as host

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "validation/https-cpu0-20261027"


class ProbeRunnerTests(unittest.TestCase):
    def replay(self, text, *, case="valid", elapsed=25.7, loaded=True, accepted=True):
        args = argparse.Namespace(serial_port="fixture", baud=921600, command=None,
                                  case=case, peer="10.17.1.17", timeout=50,
                                  label="fixture", require_workload=loaded)
        with tempfile.TemporaryDirectory() as folder:
            with patch.object(host, "OUT", Path(folder)), patch("serial.Serial"), \
                    patch.object(host, "serial_command", side_effect=["fresh seed accepted", text]), \
                    patch.object(host.time, "monotonic", side_effect=[0, elapsed]), \
                    contextlib.redirect_stdout(io.StringIO()):
                if accepted:
                    host.run(args)
                else:
                    with self.assertRaises(SystemExit):
                        host.run(args)
            return json.loads(next(Path(folder).glob("*.json")).read_text())

    def loaded_text(self):
        return (EVIDENCE / "20261027T145126Z-valid.txt").read_text()

    def test_verified_loaded_response(self):
        result = self.replay(self.loaded_text())
        self.assertTrue(result["pass"] and result["workload_pass"] and result["timing_valid"])

    def test_missing_hid_rejects_loaded_run(self):
        result = self.replay(self.loaded_text().replace("hid=785", "hid=0"), accepted=False)
        self.assertFalse(result["workload_pass"])

    def test_idle_capture_rejects_loaded_run(self):
        text = re.sub(r"\bvideo_frames=\d+", "video_frames=0", self.loaded_text())
        result = self.replay(text, accepted=False)
        self.assertFalse(result["workload_pass"] or result["workload"]["capture_active"])

    def test_storage_crc_error_rejects_run(self):
        text = self.loaded_text().replace("sd_crc_errors=0", "sd_crc_errors=1", 1)
        result = self.replay(text, accepted=False)
        self.assertFalse(result["workload_pass"])

    def test_wrong_target_clock_rejects_run(self):
        result = self.replay(self.loaded_text(), elapsed=10, accepted=False)
        self.assertFalse(result["timing_valid"])

    def test_certificate_rejections(self):
        for stamp, case in (("145009", "hostname"), ("145027", "untrusted"),
                            ("145033", "expired")):
            with self.subTest(case=case):
                text = (EVIDENCE / f"20261027T{stamp}Z-{case}.txt").read_text()
                result = self.replay(text, case=case, elapsed=7, loaded=False)
                self.assertTrue(result["pass"])
                self.assertEqual(result["result"]["body"], 0)


if __name__ == "__main__":
    unittest.main()
