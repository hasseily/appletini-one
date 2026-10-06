#!/usr/bin/env python3
"""Guard the native adapter against zero-input tones without cutting real tails.

This is a host numerical regression, not a hardware or SC-02 fidelity test.
The current-engine baseline has its separate saved-RTL parity test.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

import numpy as np
import render_ssi263 as renderer
from ssi263_host_data import make_demo, write_tables


HARNESS = r"""
#define main ssi_host_cli_main
#include "main.cpp"
#undef main

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const Tables tables = read_tables(argv[1]);
    int checks = 0, largest_tail = 0;
    for (int ff : {64, 128, 200}) {
        for (int phone = 0; phone < 64; ++phone) {
            for (bool fricative : {false, true}) {
                Baseline audio(tables);
                audio.set_native_transition_mode(true);
                audio.write(4, ff);
                audio.write(0, phone);
                audio.write(3, 0x5f);
                std::array<int, 8> codes{};
                for (int i = 0; i < 8; ++i)
                    codes[i] = (tables.native_rom[phone * 8 + i] >> 4) & 15;
                codes[4] = 15;
                codes[5] = fricative ? 0 : 15;
                codes[6] = fricative ? 15 : 0;
                audio.override_parameters(codes, true, true);
                audio.source_override(fricative ? 0 : 8192, fricative ? 8192 : 0);
                for (int i = 0; i < 50; ++i) audio.sample();
                // Stop VA/FA excitation without a phone write or filter reset.
                // A hard mute here would incorrectly erase stored filter energy.
                codes[5] = codes[6] = 0;
                audio.override_parameters(codes, true, true);
                int onset_peak = 0, late_peak = 0;
                for (int i = 0; i < 24000; ++i) {
                    const int value = std::abs(int(audio.sample()));
                    if (i < 200) onset_peak = std::max(onset_peak, value);
                    if (i >= 21600) late_peak = std::max(late_peak, value);
                }
                if (!onset_peak) {
                    std::cerr << "filter tail was erased for phone " << phone << '\n';
                    return 3;
                }
                if (late_peak > 1) {
                    std::cerr << "zero-input tone: phone " << phone << " ff " << ff
                              << " noise " << fricative << " peak " << late_peak << '\n';
                    return 4;
                }
                largest_tail = std::max(largest_tail, late_peak);
                ++checks;
            }
        }
    }
    std::cout << checks << " source-off tails retained and settled; maximum late PCM "
              << largest_tail << '\n';
}
"""


class NativeAudioPrecisionTests(unittest.TestCase):
    def test_reported_pause_whine_is_absent(self):
        with tempfile.TemporaryDirectory(prefix="ssi263_quiet_") as directory:
            output = Path(directory)
            renderer.render(make_demo("hello"), output, "transitions")
            with wave.open(str(output / "transitions.wav"), "rb") as wav:
                pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2)
            # Keep the full native VA fade; only inspect its settled tail.
            for start, end in ((.95, 1.015), (1.95, 2.015), (2.12, 2.23)):
                with self.subTest(start=start):
                    tail = pcm[round(start * 48000):round(end * 48000)]
                    self.assertLessEqual(int(np.abs(tail.astype(np.int32)).max()), 1)
            self.assertGreater(np.max(np.abs(pcm[round(.80 * 48000):round(.84 * 48000)].astype(np.int32))), 100)

    def test_source_off_preserves_then_decays_filter_energy(self):
        with tempfile.TemporaryDirectory(prefix="ssi263_tails_") as directory:
            output = Path(directory)
            write_tables(output / "tables.txt")
            (output / "tails.cpp").write_text(HARNESS)
            compiler = renderer.find_compiler()
            executable = output / ("tails.exe" if os.name == "nt" else "tails")
            command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.name == "nt":
                command.append("-static")
            command += ["-I", str(renderer.SOURCE), str(output / "tails.cpp")]
            command += [str(path) for path in sorted(renderer.SOURCE.glob("*.cpp")) if path.name != "main.cpp"]
            command += ["-o", str(executable)]
            environment = os.environ.copy()
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            compiled = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable), str(output / "tables.txt")], capture_output=True, text=True)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            print(tested.stdout.strip())


if __name__ == "__main__":
    unittest.main()
