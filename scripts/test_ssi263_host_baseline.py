#!/usr/bin/env python3
"""Compare the fast host baseline with a saved independent RTL capture.

The 120 ms, two-socket golden capture predates the host renderer. No Vivado or
ignored build artifact is needed. This checks the current hybrid engine port;
it is not a comparison against production SSI silicon.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

import ssi263_host_data as data


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "scripts" / "fixtures" / "ssi263_host" / "current_rtl_120ms"
HOST = ROOT / "scripts" / "ssi263_host"
PCM_SHA256 = "d8ab7165ad17dad877ffa281e82ff8c7edd1d024b40f62b888dfbee2384c1eff"


def compiler() -> str:
    candidates = [os.environ.get("CXX"), shutil.which("g++"), shutil.which("clang++")]
    if os.name == "nt":
        candidates.append("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/g++.exe")
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise RuntimeError("A C++17 compiler is required; set CXX to its executable path")


HARNESS = r'''
#define main ssi_host_cli_main
#include "main.cpp"
#undef main

// Replay at the saved fabric-cycle positions without running fabric cycles.
// The RTL records the previously completed output at each audio_tick. Hold one
// host sample to match that deliberate pipeline delay exactly.
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const Tables tables = read_tables(argv[1]);
    std::ifstream config(argv[2]);
    int64_t fabric_hz, clock_hz, capture_start, end_cycle;
    config >> fabric_hz >> clock_hz >> capture_start >> end_cycle;
    if (!config) return 3;
    Baseline audio[2] = {Baseline(tables, int(clock_hz)), Baseline(tables, int(clock_hz))};
    std::ifstream events(argv[3]);
    std::ofstream out(argv[4], std::ios::binary);
    int64_t event_cycle;
    int index, target, reg, value;
    events >> event_cycle >> index >> target >> reg >> value;
    int previous[2] = {0, 0};
    for (int64_t sample_number = 1; ; ++sample_number) {
        const int64_t cycle = (sample_number * fabric_hz + sample_rate - 1) / sample_rate - 1;
        if (cycle >= end_cycle) break;
        while (events && event_cycle <= cycle) {
            if (target >= 4) audio[target - 4].write(reg, value);
            events >> event_cycle >> index >> target >> reg >> value;
        }
        if (cycle >= capture_start) {
            for (int socket = 0; socket < 2; ++socket) {
                const uint16_t value = static_cast<uint16_t>(previous[socket]);
                out.put(static_cast<char>(value & 255));
                out.put(static_cast<char>(value >> 8));
            }
        }
        for (int socket = 0; socket < 2; ++socket) previous[socket] = audio[socket].sample();
    }
    return out ? 0 : 4;
}
'''


class BaselineRtlParity(unittest.TestCase):
    def test_saved_rtl_capture(self) -> None:
        metadata = json.loads(FIXTURE.with_suffix(".json").read_text())
        expected = gzip.decompress(FIXTURE.with_suffix(".pcm.gz").read_bytes())
        self.assertEqual(hashlib.sha256(expected).hexdigest(), PCM_SHA256)
        self.assertEqual(len(expected), metadata["frames"] * 4)
        self.assertEqual(metadata["frames"], 5760)
        with tempfile.TemporaryDirectory(prefix="ssi263_host_baseline_") as directory:
            temp = Path(directory)
            data.write_tables(temp / "tables.txt")
            (temp / "config.txt").write_text(" ".join(str(metadata[key]) for key in (
                "fabric_hz", "effective_xck_hz", "capture_start_fabric_cycle", "end_fabric_cycle")))
            (temp / "events.txt").write_text("\n".join(" ".join(map(str, row)) for row in metadata["events"]))
            (temp / "parity.cpp").write_text(HARNESS)
            executable = temp / ("parity.exe" if os.name == "nt" else "parity")
            command = [compiler(), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.name == "nt":
                command += ["-static", "-static-libgcc", "-static-libstdc++"]
            command += ["-I", str(HOST), str(temp / "parity.cpp")]
            command += [str(path) for path in sorted(HOST.glob("*.cpp")) if path.name != "main.cpp"]
            command += ["-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(executable), str(temp / "tables.txt"), str(temp / "config.txt"),
                            str(temp / "events.txt"), str(temp / "actual.pcm")], check=True)
            actual = (temp / "actual.pcm").read_bytes()
        self.assertEqual(len(actual), len(expected))
        if actual != expected:
            wanted = struct.unpack(f"<{len(expected) // 2}h", expected)
            got = struct.unpack(f"<{len(actual) // 2}h", actual)
            first = next(index for index, (a, b) in enumerate(zip(wanted, got)) if a != b)
            self.fail(f"RTL mismatch at frame {first // 2}, socket {first % 2}: "
                      f"expected {wanted[first]}, got {got[first]}")
        print("Baseline matches all 5,760 stereo RTL frames exactly (11,520 samples).")


if __name__ == "__main__":
    unittest.main()
