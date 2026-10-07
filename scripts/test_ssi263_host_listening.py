#!/usr/bin/env python3
"""Check paired listening pages and portable replay, without Vivado or hardware."""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
from dataclasses import replace
from html.parser import HTMLParser
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit, unquote
import zipfile

import render_ssi263 as renderer
from ssi263_host_data import DEMO_NAMES, make_demo


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.audio = []
        self.notes = []

    def handle_starttag(self, tag, attributes):
        attributes = dict(attributes)
        if tag == "audio":
            self.audio.append(attributes["src"])
        if tag == "a":
            self.notes.append(attributes["href"])


class ListeningTests(unittest.TestCase):
    executable_override: Path | None = None

    @classmethod
    def setUpClass(cls):
        cls.executable = (cls.executable_override or renderer.build_host()).resolve()
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.output = cls.root / "listening with spaces"
        arguments = ["render_ssi263.py", "--demo", "hello", "--engine", "all",
                     "--compare-ff", "232", "--output", str(cls.output),
                     "--end-seconds", "0.45", "--bundle", "--prototype-gain", "13",
                     "--voice-trim", "1700", "--articulation-reference-rate", "6"]
        with patch.object(sys, "argv", arguments), \
                patch.object(renderer, "build_host", return_value=cls.executable), \
                redirect_stdout(io.StringIO()):
            renderer.main()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_comparison_changes_only_filter_register_writes(self):
        for demo in DEMO_NAMES:
            with self.subTest(demo=demo):
                initial = make_demo(demo, filter_frequency=128)
                changed = make_demo(demo, filter_frequency=232)
                self.assertEqual(initial.xck_hz, changed.xck_hz)
                self.assertEqual(initial.duration_ticks, changed.duration_ticks)
                self.assertEqual(len(initial.events), len(changed.events))
                differences = [(a, b) for a, b in zip(initial.events, changed.events) if a != b]
                self.assertEqual(len(differences), 2)
                self.assertEqual({a.socket for a, _ in differences}, {0, 1})
                for a, b in differences:
                    self.assertEqual(a.register, 4)
                    self.assertEqual(b, replace(a, value=232))
                self.assertEqual({k: v for k, v in initial.metadata.items() if k != "filter_frequency"},
                                 {k: v for k, v in changed.metadata.items() if k != "filter_frequency"})

        original = json.loads((self.output / "trace.json").read_text())
        comparison = json.loads((self.output / "ff-232/trace.json").read_text())
        self.assertEqual(original["duration_ticks"], comparison["duration_ticks"])
        for a, b in zip(original["events"], comparison["events"]):
            self.assertEqual({**a, "value": 232} if a["register"] == 4 else a, b)

    def test_every_html_link_resolves_and_audio_query_matches_content(self):
        for page, groups in ((self.output / "listen.html", 2),
                             (self.output / "ff-232/listen.html", 1)):
            links = Links()
            links.feed(page.read_text(encoding="utf-8"))
            self.assertEqual(len(links.audio), groups * len(renderer.PROFILES))
            self.assertEqual(len(links.notes), groups * len(renderer.PROFILES))
            for address in links.audio + links.notes:
                parsed = urlsplit(address)
                self.assertFalse(parsed.scheme or parsed.netloc)
                destination = (page.parent / unquote(parsed.path)).resolve()
                self.assertTrue(destination.is_relative_to(self.output.resolve()))
                self.assertTrue(destination.is_file(), address)
                if destination.suffix == ".wav":
                    self.assertEqual(parse_qs(parsed.query), {"v": [renderer.digest(destination)[:12]]})

    def test_bundle_contains_both_groups_and_replays_exact_wavs(self):
        extracted = self.root / "portable copy with spaces"
        with zipfile.ZipFile(self.output / "SSI263-HOST-LISTEN.zip") as bundle:
            names = set(bundle.namelist())
            self.assertEqual(len(names), len(bundle.namelist()), "duplicate ZIP members")
            for prefix in ("", "ff-232/"):
                required = {prefix + name for name in ("tables.txt", "events.txt", "trace.json", "listen.html")}
                required |= {f"{prefix}{profile}.{suffix}" for profile in renderer.PROFILES for suffix in ("wav", "json")}
                self.assertTrue(required <= names)
            self.assertTrue({self.executable.name, "MODEL-NOTES.md", "README.txt"} <= names)
            bundle.extractall(extracted)

        # Exercise the actual commands shipped on Windows. Parse them into
        # argument lists so the test does not run the final interactive pause.
        if os.name == "nt":
            commands = [shlex.split(line) for line in (extracted / "render.cmd").read_text().splitlines()
                        if line.startswith('"' + self.executable.name + '"')]
        else:
            commands = [[self.executable.name, prefix + "tables.txt", prefix + "events.txt",
                         prefix + "rerender-" + profile + ".wav", profile, "6",
                         "13" if profile == "prototype" else "1", "1700" if profile == "prototype" else "16384"]
                        for prefix in ("", "ff-232/") for profile in renderer.PROFILES]
        self.assertEqual(len(commands), 2 * len(renderer.PROFILES))
        for command in commands:
            command[0] = str(extracted / self.executable.name)
            destination = extracted / command[3]
            original = destination.with_name(destination.name.removeprefix("rerender-"))
            report = json.loads(original.with_suffix(".json").read_text())
            self.assertEqual(command[4], report["profile"])
            self.assertEqual(int(command[5]), report["articulation_reference_rate"])
            if command[4] == "prototype":
                self.assertEqual(int(command[6]), report["prototype_gain"])
                self.assertEqual(int(command[7]), report["voice_trim_q16"])
            result = subprocess.run(command, cwd=extracted, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(destination.read_bytes(), original.read_bytes(), command[3])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="use an existing host executable without rebuilding")
    arguments = parser.parse_args()
    ListeningTests.executable_override = arguments.executable
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ListeningTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
