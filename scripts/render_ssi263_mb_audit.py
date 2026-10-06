#!/usr/bin/env python3
"""Render mb-audit / Tom Charlesworth's A-G SSI playback register sequences.

The fixture contains the bytes sent to the SSI, after the player's lookup.
It does not use an SC-01 sound model or regenerate any ROM/coefficient data.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import html
import json
from pathlib import Path
import wave
import zipfile

from render_ssi263 import CACHE, ROOT, build_host, digest, render
from ssi263_host_data import Event, PAL_EFFECTIVE_XCK_HZ, Trace

FIXTURE = ROOT / "scripts/fixtures/ssi263_host/mb_audit_phrases.json"
PROFILES = ("baseline", "prototype")


def duration_ticks(durphon: int, rate: int) -> int:
    return 4096 * (16 - rate) * (4 - (durphon >> 6))


def make_trace(fixture: dict, phrase: dict, rate: int | None = None) -> Trace:
    """Schedule nominal SSI requests; this is not a 6502/IRQ simulation.

    Initialization and shutdown preserve bus write order. Small write spacing
    follows the source instructions, but IRQ entry, competing timer IRQs and
    page-cross penalties are omitted. Each completed phoneme triggers the next
    row immediately; its register-zero write starts the next duration.
    """
    clock = PAL_EFFECTIVE_XCK_HZ
    events: list[Event] = []
    segments = []

    def write(tick: int, register: int, value: int, index: int = -1) -> None:
        # Replay the same stimulus in both channels, as separate model states.
        for socket in (0, 1):
            events.append(Event(tick, socket, register, value, index))

    if rate is not None:
        if type(rate) is not int or not 0 <= rate <= 15:
            raise ValueError("RATE must be 0..15")
        attributes = list(fixture["translated_player"]["attributes"])
        attributes[2] = (rate << 4) | (attributes[2] & 15)
        # KickSSI263: 4,3,2,1; CTL high; C0; CTL low (function 3).
        for index, register in enumerate((4, 3, 2, 1)):
            write(15 * index, register, attributes[register])
        write(63, 3, attributes[3] | 0x80)
        write(73, 0, 0xC0)
        write(87, 3, attributes[3] & 0x7F)
        tick = 87 + duration_ticks(0xC0, rate)
        for index, value in enumerate(phrase["native_ssi_durphon"]):
            if type(value) is not int or not 0 <= value < 64:
                raise ValueError("translated player must write a native SSI phone with DUR=0")
            write(tick, 0, value, index)
            end = tick + duration_ticks(value, rate)
            segments.append({"phone": value, "durphon": value,
                             "start_tick": tick, "end_tick": end})
            tick = end
        ff = attributes[4]
        timing_note = "Initial C0 request then DUR=0 SSI requests; 6502 ISR latency omitted."
    else:
        # Original mb-audit ClassicAdv writes all five registers on each IRQ.
        # Its startup rate is not set by KickSSI263. Use the cold model's R=0
        # for the silent kick; the first phrase row sets its own RATE=$B.
        write(0, 3, 0x80)
        write(10, 0, 0xC0)
        write(20, 3, 0)
        tick = 20 + duration_ticks(0xC0, 0)
        for index, row in enumerate(phrase["register_rows"]):
            if len(row) != 5 or any(type(v) is not int or not 0 <= v <= 255 for v in row):
                raise ValueError("ClassicAdv rows must have five register bytes")
            for offset, register in enumerate((4, 3, 2, 1, 0)):
                write(tick + 16 * offset, register, row[register], index)
            start = tick + 64
            end = start + duration_ticks(row[0], row[2] >> 4)
            segments.append({"phone": row[0] & 63, "durphon": row[0],
                             "start_tick": start, "end_tick": end})
            tick = end
        ff = phrase["register_rows"][0][4]
        timing_note = ("Original five-register rows, nominal SSI requests without ISR latency. "
                       "Silent startup kick uses model reset RATE=0; prior hardware register state is unknown.")

    # The five-zero sentinel calls DisableSSI263; it is not a speech row.
    write(tick, 3, 0x80)
    write(tick + 10, 0, 0)
    write(tick + 20, 3, 0)
    trace = Trace(events, clock, tick + 20 + clock // 4, {
        "name": phrase["id"], "description": phrase["text"],
        "source_label": phrase["label"], "source": fixture["provenance"],
        "fixture_sha256": digest(FIXTURE), "rate": rate,
        "filter_frequency": ff, "segments": segments,
        "timing": timing_note,
        "channel_order": ["SSI stream, socket 0", "same SSI stream, socket 1"],
        "ssi_writes": len(events),
    })
    trace.validate()
    return trace


def playlist(out: Path, directories: list[Path], profile: str, destination: Path) -> None:
    """Join independently reset clips, retaining their gain and adding 0.35s gaps."""
    with wave.open(str(destination), "wb") as output:
        output.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        for index, directory in enumerate(directories):
            with wave.open(str(out / directory / f"{profile}.wav"), "rb") as source:
                if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (2, 2, 48000):
                    raise ValueError("unexpected playlist WAV format")
                if index:
                    output.writeframes(b"\0" * (16800 * 4))
                output.writeframes(source.readframes(source.getnframes()))


def write_page(out: Path, groups: list[dict], classic: dict) -> Path:
    def players(directory: str, reports: dict) -> str:
        cards = []
        for profile, label in (("baseline", "Current-engine baseline"),
                               ("prototype", "Native SSI / prototype candidate")):
            filename = f"{directory}/{profile}.wav"
            sha = reports[profile]["wav_sha256"]
            cards.append(f'<div><h3>{label}</h3><audio controls preload="none" '
                         f'src="{filename}?v={sha[:12]}"></audio>'
                         f'<a href="{filename}" download>WAV</a> · '
                         f'<a href="{directory}/{profile}.json">Model notes</a></div>')
        return '<div class="pair">' + ''.join(cards) + '</div>'

    sections = []
    for group in groups:
        rate = group["rate"]
        prefix = f"rate-{rate:X}"
        playlist_reports = {p: {"wav_sha256": digest(out / prefix / f"{p}.wav")} for p in PROFILES}
        # Playlist model notes use the manifest instead of an individual clip report.
        for p in PROFILES:
            (out / prefix / f"{p}.json").write_text(json.dumps(group, indent=2) + "\n")
        content = '<section><h2>All seven phrases · A–G</h2>' + players(prefix, playlist_reports) + '</section>'
        for entry in group["phrases"]:
            content += (f'<section><h2>{entry["id"]}. {html.escape(entry["text"])}</h2>'
                        + players(entry["directory"], entry["reports"]) + '</section>')
        hidden = '' if rate == 11 else ' hidden'
        sections.append(f'<div class="rate-group" data-rate="{rate}"{hidden}>{content}</div>')
    page = out / "listen.html"
    page.write_text('''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SSI · mb-audit phrases</title><style>
body{max-width:1050px;margin:42px auto;padding:0 24px;background:#171b20;color:#e3e9ef;font:16px/1.5 system-ui}
section{padding:18px 24px;margin:20px 0;background:#232a32;border-radius:12px}
h1{margin-bottom:8px}h2{font-size:21px}h3{font-size:16px;font-weight:500}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:28px}audio{display:block;width:100%;margin:8px 0}
a{color:#9ecfff}p,details{color:#bdc8d3}select{font:inherit;padding:8px;background:#232a32;color:inherit}
@media(max-width:650px){.pair{grid-template-columns:1fr;gap:10px}}
</style><h1>mb-audit speech phrases</h1>
<p>Compare the current engine with the native SSI candidate. Same SSI register stream and fixed model gains;
no normalization or sound tuning. Each clip starts from a fresh model state.</p>
<p><label>Seven-phrase playback rate: <select id="rate">
<option value="10">$A — player default</option><option value="11" selected>$B — recorded example</option>
<option value="12">$C — faster recorded example</option></select></label></p>
''' + ''.join(sections) + '<section><h2>Classic Adventure · original mb-audit test</h2>'
        '<p>This test uses its own original register values and duration codes.</p>'
        + players(classic["directory"], classic["reports"]) + '''</section>
<details><summary>Source and timing notes</summary>
<p>A–G come from Tom Charlesworth's <a href="https://github.com/tomcw/play-sc01-using-ssi263">SSI phrase player</a>.
These replay the native SSI bytes its driver sends after its phrase lookup. The synthesis uses the SSI model.
Classic Adventure comes from <a href="https://github.com/tomcw/mb-audit">mb-audit</a>.</p>
<p>PAL effective XCK is 1,015,625 Hz. Phonemes advance at nominal SSI request times;
6502 interrupt latency is omitted. A–G retain FF=$E9, ART=5, AMP=12 and the player's pitch.
The same stream feeds both stereo channels. The prototype's cold noise route now starts on FRIC1;
this is a startup assumption based on the listening report, pending the physical capture.</p>
<p>The two models interpret FF differently. Each receives the original byte; no compensating filter adjustment
has been made. Phasor mixing and analog output response are not modeled.</p>
<p><a href="manifest.json">Source hashes, traces and render manifest</a></p></details>
<p><a href="SSI263-MB-AUDIT-LISTEN.zip">Download all clips and register traces</a></p>
<script>
document.getElementById('rate').addEventListener('change',e=>{
 document.querySelectorAll('audio').forEach(a=>a.pause());
 document.querySelectorAll('.rate-group').forEach(g=>g.hidden=g.dataset.rate!==e.target.value);
});
document.querySelectorAll('audio').forEach(a=>a.addEventListener('play',()=>{
 document.querySelectorAll('audio').forEach(b=>{if(a!==b)b.pause()});
}));
</script></html>''', encoding="utf-8")
    return page


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=CACHE / "mb_audit")
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    out = args.output.resolve()
    executable = build_host()

    def run(phrase: dict, directory: Path, rate: int | None = None) -> dict:
        trace = make_trace(fixture, phrase, rate)
        reports = {p: render(trace, out / directory, p, executable) for p in PROFILES}
        (out / directory / "trace.json").write_text(json.dumps({
            "effective_clock_hz": trace.xck_hz, "duration_ticks": trace.duration_ticks,
            "metadata": trace.metadata, "events": [asdict(e) for e in trace.events]}, indent=2) + "\n")
        print(f'{directory}: {reports["prototype"]["audio_seconds"]:.2f}s')
        return {"id": phrase["id"], "text": phrase["text"],
                "directory": directory.as_posix(), "reports": reports}

    groups = []
    for rate in (10, 11, 12):
        entries = [run(p, Path(f"rate-{rate:X}") / p["id"], rate)
                   for p in fixture["translated_player"]["phrases"]]
        for profile in PROFILES:
            playlist(out, [Path(e["directory"]) for e in entries], profile,
                     out / f"rate-{rate:X}" / f"{profile}.wav")
        groups.append({"rate": rate, "phrases": entries})
    classic = run(fixture["classic_adventure"], Path("classic"))
    (out / "manifest.json").write_text(json.dumps({"source": fixture, "groups": groups,
        "classic": classic}, indent=2) + "\n")
    print(write_page(out, groups, classic))
    with zipfile.ZipFile(out / "SSI263-MB-AUDIT-LISTEN.zip", "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(out.rglob("*")):
            if path.is_file() and path.suffix in (".wav", ".json", ".html", ".txt"):
                bundle.write(path, path.relative_to(out).as_posix())
    print(out / "SSI263-MB-AUDIT-LISTEN.zip")


if __name__ == "__main__":
    main()
