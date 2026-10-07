#!/usr/bin/env python3
"""Build an offline PAL-01 capture/model listening and measurement page.

Uses one recording-wide AY-marker timing fit and the isolation-derived socket
mapping. No per-phone alignment, gain fitting, EQ, or capture resampling occurs.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import shutil
import wave

import numpy as np

import analyze_ssi263_capture as capture
import render_ssi263 as renderer
from ssi263_host_data import load_calibration

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ("baseline", "prototype")
DEMO_LABEL = "SYNTHETIC TOOLING DEMO — not a hardware recording or an accuracy result"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_json(value) -> str:
    """An inline script must not accept a closing tag from input metadata."""
    return json.dumps(value, separators=(",", ":"), allow_nan=False).replace(
        "<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026").replace(
            "\u2028", "\\u2028").replace("\u2029", "\\u2029")


def interval(samples: np.ndarray, rate: int, begin: float, end: float) -> tuple[np.ndarray, int, str]:
    first, last = round(begin * rate), round(end * rate)
    if last <= first or last <= 0 or first >= len(samples):
        return samples[:0], max(0, min(len(samples), first)), "outside_capture"
    status = "complete" if first >= 0 and last <= len(samples) else "partial"
    first, last = max(0, first), min(len(samples), last)
    return samples[first:last], first, status


def spectrum(samples: np.ndarray, rate: int, max_points: int = 900) -> dict:
    """One-sided Hann FFT peak amplitude, on the original sample-rate axis.

    The plot keeps the largest bin in each small adjacent-bin group. Raw level
    metrics use every sample, not the plot reduction. DC removal is explicit.
    """
    if len(samples) < 3:
        return {"points": [], "resolution_hz": None, "peak_hz": None}
    centered = samples - samples.mean()
    taper = np.hanning(len(samples))
    amplitudes = 2 * np.abs(np.fft.rfft(centered * taper)) / max(float(taper.sum()), 1e-30)
    amplitudes[0] *= .5
    if len(samples) % 2 == 0:
        amplitudes[-1] *= .5
    frequencies = np.fft.rfftfreq(len(samples), 1 / rate)
    valid = np.flatnonzero((frequencies >= 20) & (frequencies <= min(20000, rate / 2)))
    if not len(valid):
        return {"points": [], "resolution_hz": rate / len(samples), "peak_hz": None}
    points = []
    for group in np.array_split(valid, min(max_points, len(valid))):
        index = group[int(np.argmax(amplitudes[group]))]
        level = 20 * math.log10(max(float(amplitudes[index]), 1e-7))
        points.append([round(float(frequencies[index]), 4), round(level, 4)])
    peak = valid[int(np.argmax(amplitudes[valid]))]
    return {"points": points, "resolution_hz": rate / len(samples),
            "peak_hz": float(frequencies[peak]) if amplitudes[peak] > 1e-12 else None,
            "peak_amplitude_dbfs": capture.db(float(amplitudes[peak]))}


def envelope(samples: np.ndarray, rate: int, first_sample: int,
             nominal_origin: float, offset: float = 0, scale: float = 1) -> list:
    # Two millisecond RMS bins, widened only for long context views. No gain
    # normalization: all three traces share one dBFS plot scale.
    width = max(1, round(rate * .002), math.ceil(len(samples) / 1200))
    result = []
    for first in range(0, len(samples), width):
        block = samples[first:first + width]
        rms = float(np.sqrt(np.mean(block * block)))
        recorded_time = (first_sample + first + len(block) / 2) / rate
        time = (recorded_time - offset) / scale - nominal_origin
        result.append([round(time, 7), round(20 * math.log10(max(rms, 1e-7)), 4)])
    return result


def measure_series(samples: np.ndarray, rate: int, clip_level: float, channel: int,
                   context: tuple[float, float], candidate: tuple[float, float],
                   segment_start: float, offset: float = 0, scale: float = 1) -> dict:
    context_times = tuple(offset + scale * value for value in context)
    candidate_times = tuple(offset + scale * value for value in candidate)
    contextual, first, context_status = interval(samples, rate, *context_times)
    selected, _, window_status = interval(samples, rate, *candidate_times)
    result = {"sample_rate": rate, "channel": channel + 1,
              "context_status": context_status, "window_status": window_status,
              "context_clip": [max(0, context_times[0]), min(len(samples) / rate, context_times[1])],
              "window_clip": [max(0, candidate_times[0]), min(len(samples) / rate, candidate_times[1])],
              "envelope": envelope(contextual[:, channel], rate, first, segment_start, offset, scale)
                          if len(contextual) else [],
              "metrics": None, "spectrum": {"points": [], "resolution_hz": None, "peak_hz": None}}
    # A partial candidate is not silently treated as the requested window.
    if window_status == "complete" and len(selected) >= 3:
        result["metrics"] = capture.window_metrics(selected[:, channel:channel + 1], rate, clip_level)[0]
        result["spectrum"] = spectrum(selected[:, channel], rate)
    return result


def compare_windows(manifest: dict, analysis: dict, recorded: tuple,
                    models: dict[str, tuple], context_seconds: float = .08) -> list[dict]:
    recording_rate, recording_samples, recording_info = recorded
    alignment = analysis.get("alignment")
    mapping = analysis.get("socket_mapping", {})
    mapped = mapping.get("status") == "distinct"
    tick = manifest["clock"]["tick_seconds"]
    windows = []
    for segment in manifest["segments"]:
        for number, window in enumerate(segment["candidate_windows"]):
            start = segment.get("start_tick", window["start_tick"]) * tick
            stop = segment.get("end_tick", window["end_tick"]) * tick
            candidate = (window["start_tick"] * tick, window["end_tick"] * tick)
            context = (max(0, start - context_seconds), min(manifest["duration_seconds"], stop + context_seconds))
            flags = ["Candidate settling is unverified; this page does not fit the chip model."]
            if segment["kind"] == "ff-sweep" or candidate[1] - candidate[0] <= .0501:
                label = "Short FF window" if segment["kind"] == "ff-sweep" else "Short candidate window"
                flags.append(label + ": 25 ms marker-edge uncertainty can cross adjacent settings. Gross behavior only.")
            if not alignment:
                flags.append("Missing or ambiguous AY markers: no matched recording interval is available.")
            elif not mapped:
                flags.append("Socket/channel mapping is unresolved: recorded channels are not assigned by guesswork.")
            item = {"id": f"{segment['segment_id']}:{number}", "segment_id": segment["segment_id"],
                    "kind": segment["kind"], "purpose": window["purpose"],
                    "settings": segment["settings"], "notes": segment.get("notes", []),
                    "segment_seconds": [start, stop], "context_seconds": list(context),
                    "candidate_seconds": list(candidate), "flags": flags, "targets": {}}
            for target in (4, 5):
                series = {}
                if alignment and mapped:
                    channel = mapping["isolations"][str(target)]["candidate_channel"] - 1
                    series["capture"] = measure_series(recording_samples, recording_rate,
                        recording_info["clip_level"], channel, context, candidate, start,
                        alignment["offset_seconds"], alignment["time_scale"])
                else:
                    series["capture"] = None
                for profile, (rate, samples, info) in models.items():
                    series[profile] = measure_series(samples, rate, info["clip_level"],
                        target - 4, context, candidate, start)
                item["targets"][str(target)] = series
            windows.append(item)
    return windows


def write_pcm24(path: Path, rate: int, samples: np.ndarray) -> None:
    if not np.isfinite(samples).all() or np.max(np.abs(samples), initial=0) >= 1:
        raise ValueError("Synthetic demo exceeds PCM range; refusing to clip or normalize it")
    values = np.rint(samples * 8388608).astype(np.int32).ravel()
    octets = np.empty((len(values), 3), dtype=np.uint8)
    for byte in range(3):
        octets[:, byte] = (values >> (8 * byte)) & 255
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2); output.setsampwidth(3); output.setframerate(rate)
        output.writeframes(octets.tobytes())


def synthetic_capture(model: np.ndarray, rate: int, manifest: dict, path: Path,
                      offset: float = .73, scale: float = 1.0006) -> dict:
    """Known synthetic recorder offset/drift/swapping; never hardware evidence."""
    length = math.ceil((offset + scale * manifest["duration_seconds"] + .3) * rate)
    nominal = (np.arange(length, dtype=np.float64) / rate - offset) / scale
    model_time = np.arange(len(model), dtype=np.float64) / rate
    result = np.empty((length, 2), dtype=np.float64)
    for channel in (0, 1):
        result[:, channel] = np.interp(nominal, model_time, model[:, 1 - channel], left=0, right=0)
    tick = manifest["clock"]["tick_seconds"]
    for start_tick, duration_ticks, hz in ((100, 20, 1000), (140, 10, 1500),
                                          (11650, 10, 1500), (11690, 20, 1000), (11730, 10, 1500)):
        start, end = start_tick * tick, (start_tick + duration_ticks) * tick
        first = np.searchsorted(nominal, start); last = np.searchsorted(nominal, end)
        tone = .12 * np.sin(2 * np.pi * hz * (nominal[first:last] - start))
        result[first:last, 0] += tone
        result[first:last, 1] -= tone
    write_pcm24(path, rate, result)
    return {"label": DEMO_LABEL, "source": "fresh baseline model plus ideal AY markers",
            "known_offset_seconds": offset, "known_time_scale": scale,
            "known_drift_ppm": (scale - 1) * 1e6, "known_mapping": {"4": 2, "5": 1},
            "purpose": "Exercise alignment, channel mapping, plots and playback; not chip validation."}


def write_audio_payload(path: Path, source: Path, key: str) -> None:
    # Local script loads work when index.html is opened with file://. No fetch,
    # CDN, server or Internet connection is needed. WAV bytes stay unchanged.
    encoded = base64.b64encode(source.read_bytes()).decode("ascii")
    path.write_text(f"window.SSI_AUDIO[{safe_json(key)}]={safe_json(encoded)};\n", encoding="ascii")


PAGE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SSI capture comparison</title><style>
:root{color-scheme:dark;--bg:#111821;--panel:#1b2734;--muted:#a6b6c7;--line:#344557;--capture:#ffc16b;--baseline:#db9dff;--prototype:#67dfd0}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:#eaf1f8;font:15px/1.5 system-ui,sans-serif}
main{max-width:1440px;margin:auto;padding:30px 30px 60px}h1{font-size:30px;margin:0}h2{font-size:20px;margin:0 0 12px}h3{font-size:16px;margin:0 0 8px}p{margin:8px 0;color:var(--muted)}a{color:#b0d7ff}button,select,input{font:inherit;color:inherit;background:#243547;border:1px solid #50657a;border-radius:7px;padding:8px 11px}button{cursor:pointer}button:hover{background:#334d65}button:disabled{opacity:.4;cursor:default}input{min-width:230px}select{max-width:100%}.top{display:flex;justify-content:space-between;gap:20px;align-items:center}.badge{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91ddcf}.banner{padding:15px 18px;margin:20px 0;border:1px solid #9a763e;border-radius:9px;background:#342c21;color:#ffddaa}.banner.demo{font-weight:700;background:#3e2d44;color:#f1c7ff;border-color:#976fa5}.facts{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:20px 0}.fact,.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px}.fact span{display:block;font-size:12px;color:var(--muted)}.fact strong{font-size:17px}.controls{display:grid;grid-template-columns:1fr 2fr auto;gap:12px;align-items:end;margin:22px 0}.controls label{display:block;font-size:12px;color:var(--muted);margin-bottom:4px}.controls input,.controls select{width:100%}.nav{display:flex;gap:7px;margin-top:12px}.grid{display:grid;grid-template-columns:1.2fr 1fr;gap:18px}.full{grid-column:1/-1}.legend{display:flex;gap:20px;font-size:13px;margin:8px 0 12px}.legend i{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}.plot{width:100%;height:300px;display:block}.players{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.player{border-top:3px solid var(--line)}.player button{margin:8px 5px 0 0;font-size:13px}.player .time{font-size:12px;color:var(--muted)}table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:right;padding:8px;border-bottom:1px solid var(--line)}th:first-child,td:first-child{text-align:left}th{font-weight:500;color:var(--muted)}.small{font-size:12px;color:var(--muted)}#flags li{margin:6px 0;color:#e9c88f}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.5 ui-monospace,monospace;color:var(--muted)}details summary{cursor:pointer}#status{min-height:24px;color:#ace4da}.footer{margin-top:24px}.plot-note{font-size:12px;color:var(--muted)}@media(max-width:900px){main{padding:18px}.facts{grid-template-columns:1fr 1fr}.controls,.grid{grid-template-columns:1fr}.players{grid-template-columns:1fr}.full{grid-column:auto}.top{display:block}.plot{height:260px}}
</style></head><body><main>
<div class="top"><div><div class="badge">SSI-263 · PAL-01 · Offline comparison</div><h1>Listen, then inspect the evidence</h1><p>One capture, one global timing map. Raw levels; no gain fitting or EQ.</p></div><a href="comparison.json">Full measurements & provenance</a></div>
<div id="identity" class="banner"></div><div class="facts"><div class="fact"><span>Recording status</span><strong id="quality"></strong></div><div class="fact"><span>Global timing drift</span><strong id="drift"></strong></div><div class="fact"><span>Socket mapping</span><strong id="mapping"></strong></div><div class="fact"><span>Timing uncertainty</span><strong>±25 ms at marker edges</strong></div></div>
<div class="panel"><details open><summary>Capture quality and comparison limits</summary><ul id="qualityFlags"></ul><p class="small">Timing drift combines host and recorder timing; it does not measure the chip clock. Candidate windows remain unverified. Hardware audio keeps its recorded pitch and duration.</p></details></div>
<div class="controls"><div><label for="search">Filter segments</label><input id="search" type="search" placeholder="Phone, label, FF, transition…"></div><div><label for="window">Segment / candidate window</label><select id="window"></select></div><div><label for="target">SSI socket</label><select id="target"><option value="4">Target 4 · secondary SSI</option><option value="5">Target 5 · primary SSI</option></select></div></div>
<div class="nav"><button id="previous">← Previous</button><button id="next">Next →</button><button id="stop">Stop audio</button><span id="position" class="small"></span></div><p id="status" aria-live="polite"></p>
<div class="grid"><section class="panel full"><h2 id="title"></h2><p id="purpose"></p><ul id="flags"></ul><div class="players" id="players"></div></section>
<section class="panel"><h2>Onset and whole-segment envelope</h2><div class="legend"><span><i style="background:var(--capture)"></i>Recording</span><span><i style="background:var(--baseline)"></i>Current baseline</span><span><i style="background:var(--prototype)"></i>Prototype candidate</span></div><canvas class="plot" id="envelope"></canvas><p class="plot-note"><strong>Onset timing is uncertain by ±25 ms</strong> (amber band), so this view cannot establish millisecond attack alignment. RMS uses normally 2 ms bins. Time is relative to segment start; gray shading marks the candidate window. The recording's time labels use the global affine map.</p></section>
<section class="panel"><h2>Candidate-window spectrum</h2><div class="legend"><span><i style="background:var(--capture)"></i>Recording</span><span><i style="background:var(--baseline)"></i>Baseline</span><span><i style="background:var(--prototype)"></i>Prototype</span></div><canvas class="plot" id="spectrum"></canvas><p class="plot-note">Hann FFT peak amplitude in dBFS; DC removed for this plot only. Original sample-rate frequency axes, with no drift correction. The strongest bin in each small adjacent-bin group is drawn.</p></section>
<section class="panel full"><h2>Raw candidate-window measurements</h2><div style="overflow:auto"><table><thead><tr><th>Signal</th><th>RMS dBFS</th><th>AC RMS dBFS</th><th>Peak dBFS</th><th>DC</th><th>Rail samples</th><th>Centroid Hz</th><th>FFT resolution Hz</th></tr></thead><tbody id="metrics"></tbody></table></div><p class="small">No per-window level matching. Raw recording gain and card/output response can differ from the software model. Rail counts include float overrange.</p></section>
<section class="panel full"><details><summary>Settings, notes and source files</summary><pre id="settings"></pre><p><a href="recording.wav">Original recording WAV</a> · <a href="models/baseline.wav">Continuous baseline WAV</a> · <a href="models/prototype.wav">Continuous prototype WAV</a> · <a href="capture_analysis.json">Capture analysis</a> · <a href="models/baseline.json">Baseline provenance</a> · <a href="models/prototype.json">Prototype provenance</a></p></details></section></div>
<p class="footer small">Audio starts only when you press Play. Playback gain is unity. Each clip keeps its source's own duration and pitch. Browser/device output may resample to the playback device; all plotted measurements use the untouched input samples.</p>
</main><script id="data" type="application/json">__DATA__</script><script>
'use strict';const D=JSON.parse(document.getElementById('data').textContent);window.SSI_AUDIO={};window.SSI_WINDOWS={};
const $=id=>document.getElementById(id), names={capture:'Recording',baseline:'Current baseline',prototype:'Prototype candidate'}, colors={capture:'#ffc16b',baseline:'#db9dff',prototype:'#67dfd0'};
let selected=null, currentTarget='4', player=null, audioContext=null, token=0,selectionToken=0;const decoded={},loading={},windowLoads={};
const fmt=(x,n=2)=>x===null||x===undefined?'—':Number(x).toFixed(n);const esc=x=>String(x);function li(parent,text){const e=document.createElement('li');e.textContent=text;parent.appendChild(e)}
$('identity').textContent=D.synthetic?D.synthetic.label:'Hardware recording: '+D.recording_name;if(D.synthetic)$('identity').classList.add('demo');
$('quality').textContent=D.analysis.status.replaceAll('_',' ');const align=D.analysis.alignment;
$('drift').textContent=align?fmt(align.drift_ppm,1)+' ppm':'Not established';const map=D.analysis.socket_mapping;
$('mapping').textContent=map?.status==='distinct'?'4 → ch '+map.isolations['4'].candidate_channel+' · 5 → ch '+map.isolations['5'].candidate_channel:'Unresolved';
const qf=$('qualityFlags');for(const f of D.analysis.flags)li(qf,f);for(const f of D.model_flags||[])li(qf,f);if(!D.analysis.flags.length)li(qf,'No automated capture-quality flag. This does not verify settling or physical accuracy.');
if(align)li(qf,'One global fit: offset '+fmt(align.offset_seconds,6)+' s; maximum marker residual '+fmt(1000*align.max_marker_residual_seconds,2)+' ms.');
li(qf,'Recorder: '+D.analysis.wav.sample_rate.toLocaleString()+' Hz, '+D.analysis.wav.container_bits+'-bit '+D.analysis.wav.encoding+'. Models: 48,000 Hz; all preceding register state is replayed.');
if(D.synthetic)li(qf,'Known demo construction: swapped channels, '+D.synthetic.known_offset_seconds+' s offset, '+fmt(D.synthetic.known_drift_ppm,1)+' ppm drift. Any visual match is synthetic.');
function stop(){++token;if(player){try{player.stop()}catch(e){}player=null}$('status').textContent=''}$('stop').onclick=stop;
function populate(){const term=$('search').value.toLowerCase();const prior=$('window').value;$('window').replaceChildren();for(const w of D.windows){const label=w.segment_id+' · '+w.kind+' · '+(w.settings.phone_label||'');if(!label.toLowerCase().includes(term))continue;const o=document.createElement('option');o.value=w.id;o.textContent=label;$('window').appendChild(o)}if([...$('window').options].some(o=>o.value===prior))$('window').value=prior;choose()}
function clearSelection(message){selected=null;$('title').textContent=message;$('purpose').textContent='';$('players').replaceChildren();$('metrics').replaceChildren();$('flags').replaceChildren();for(const id of ['envelope','spectrum']){const c=$(id);c.getContext('2d').clearRect(0,0,c.width,c.height)}$('position').textContent=$('window').options.length+' matching windows';$('status').textContent=message}
async function loadWindow(meta){if(window.SSI_WINDOWS[meta.id])return window.SSI_WINDOWS[meta.id];if(!windowLoads[meta.id])windowLoads[meta.id]=new Promise((resolve,reject)=>{const s=document.createElement('script');s.src=meta.plot_asset;s.onload=()=>{s.remove();const w=window.SSI_WINDOWS[meta.id];w?resolve(w):reject(Error('Missing window data'))};s.onerror=()=>reject(Error('Could not load local window data'));document.body.appendChild(s)});return windowLoads[meta.id]}
async function choose(){stop();const own=++selectionToken,meta=D.windows.find(w=>w.id===$('window').value);currentTarget=$('target').value;clearSelection(meta?'Loading selected window…':'No matching segment.');if(!meta)return;try{const full=await loadWindow(meta);if(own!==selectionToken)return;selected=full;$('status').textContent='';draw()}catch(error){if(own===selectionToken)clearSelection(error.message)}}
$('search').oninput=populate;$('window').onchange=choose;$('target').onchange=choose;for(const [id,step]of[['previous',-1],['next',1]])$(id).onclick=()=>{const s=$('window');s.selectedIndex=Math.max(0,Math.min(s.options.length-1,s.selectedIndex+step));choose()};
function plot(id,series,spectral=false){const canvas=$(id),ratio=window.devicePixelRatio||1;canvas.width=canvas.clientWidth*ratio;canvas.height=canvas.clientHeight*ratio;const g=canvas.getContext('2d');g.scale(ratio,ratio);const width=canvas.clientWidth,height=canvas.clientHeight,left=53,right=13,top=16,bottom=34,w=width-left-right,h=height-top-bottom;
const a=selected.context_seconds[0]-selected.segment_seconds[0],b=selected.context_seconds[1]-selected.segment_seconds[0];const x=v=>left+(spectral?Math.log(v/20)/Math.log(1000):(v-a)/(b-a))*w;let max=0;for(const s of Object.values(series))if(s)for(const p of(spectral?s.spectrum.points:s.envelope))max=Math.max(max,p[1]);max=Math.ceil(max/6)*6;const min=spectral?-120:-100,y=v=>top+(max-Math.max(min,v))/(max-min)*h;
g.fillStyle='#16212d';g.fillRect(0,0,width,height);if(!spectral){g.fillStyle='#54647755';g.fillRect(x(selected.candidate_seconds[0]-selected.segment_seconds[0]),top,(selected.candidate_seconds[1]-selected.candidate_seconds[0])/(b-a)*w,h);const u0=Math.max(left,x(-.025)),u1=Math.min(width-right,x(.025));if(u1>u0){g.fillStyle='#ffc16b25';g.fillRect(u0,top,u1-u0,h)}}g.font='11px system-ui';g.lineWidth=1;
for(let db=min;db<=max;db+=20){g.strokeStyle='#344557';g.beginPath();g.moveTo(left,y(db));g.lineTo(width-right,y(db));g.stroke();g.fillStyle='#a6b6c7';g.fillText(db.toString(),8,y(db)+4)}const ticks=spectral?[20,50,100,200,500,1000,2000,5000,10000,20000]:Array.from({length:6},(_,i)=>a+(b-a)*i/5);for(const v of ticks){g.fillStyle='#a6b6c7';g.fillText(spectral?(v>=1000?v/1000+'k':v):v.toFixed(2),x(v)-12,height-10)}
if(!spectral&&a<=0&&b>=0){g.strokeStyle='#d7e3ef55';g.setLineDash([3,4]);g.beginPath();g.moveTo(x(0),top);g.lineTo(x(0),top+h);g.stroke();g.setLineDash([])}g.save();g.beginPath();g.rect(left,top,w,h);g.clip();for(const key of ['baseline','prototype','capture']){const s=series[key];if(!s)continue;const points=spectral?s.spectrum.points:s.envelope;g.strokeStyle=colors[key];g.lineWidth=key==='capture'?1.8:1.2;g.beginPath();let began=false;for(const [t,v]of points){if(spectral&&(t<20||t>20000))continue;if(!began){g.moveTo(x(t),y(v));began=true}else g.lineTo(x(t),y(v))}g.stroke()}g.restore()}
async function loadAudio(key){if(decoded[key])return decoded[key];if(loading[key])return loading[key];loading[key]=(async()=>{await new Promise((resolve,reject)=>{const s=document.createElement('script');s.src='media/'+key+'.js';s.onload=()=>{s.remove();resolve()};s.onerror=()=>reject(Error('Could not load local audio asset'));document.body.appendChild(s)});const raw=atob(window.SSI_AUDIO[key]);delete window.SSI_AUDIO[key];const bytes=new Uint8Array(raw.length);for(let i=0;i<raw.length;i++)bytes[i]=raw.charCodeAt(i);decoded[key]=await audioContext.decodeAudioData(bytes.buffer);return decoded[key]})();return loading[key]}
async function play(key,which){stop();const own=token;if(!selected){$('status').textContent='Select a matching window before playback.';return}const s=selected.targets[currentTarget]?.[key];if(!s)return;try{audioContext??=new(window.AudioContext||window.webkitAudioContext)();await audioContext.resume();$('status').textContent='Loading '+names[key]+'…';const full=await loadAudio(key);if(own!==token)return;const bounds=s[which+'_clip'];const begin=Math.max(0,Math.round(bounds[0]*full.sampleRate)),end=Math.min(full.length,Math.round(bounds[1]*full.sampleRate));if(end<=begin)throw Error('This interval is outside the source');const mono=audioContext.createBuffer(1,end-begin,full.sampleRate);mono.copyToChannel(full.getChannelData(s.channel-1).subarray(begin,end),0);player=audioContext.createBufferSource();player.buffer=mono;player.connect(audioContext.destination);player.start();$('status').textContent='Playing '+names[key]+', '+which+', channel '+s.channel+' · '+fmt((end-begin)/full.sampleRate,3)+' s · unity gain';player.onended=()=>{if(own===token)$('status').textContent='Playback finished.'}}catch(error){$('status').textContent='Playback unavailable: '+error.message+'. Open the original WAV link below.'}}
function draw(){const series=selected.targets[currentTarget];$('title').textContent=selected.segment_id+' · target '+currentTarget;$('purpose').textContent=selected.purpose+' · nominal '+fmt(selected.segment_seconds[0],3)+'–'+fmt(selected.segment_seconds[1],3)+' s';$('position').textContent=($('window').selectedIndex+1)+' / '+$('window').options.length+' shown · '+D.windows.length+' total windows';$('flags').replaceChildren();for(const f of selected.flags)li($('flags'),f);const targets=selected.settings.targets;if(targets&&!targets.includes(Number(currentTarget)))li($('flags'),'This segment does not drive target '+currentTarget+'; its quiet/leakage channel is shown.');if(series.capture&&(series.capture.context_status!=='complete'||series.capture.window_status!=='complete'))li($('flags'),'This recording does not cover the full selected context/window. Missing samples are not filled.');$('players').replaceChildren();$('metrics').replaceChildren();for(const key of ['capture','baseline','prototype']){const s=series[key],card=document.createElement('div');card.className='panel player';card.style.borderTopColor=colors[key];const heading=document.createElement('h3');heading.textContent=names[key];card.appendChild(heading);const note=document.createElement('div');note.className='time';note.textContent=s?s.sample_rate.toLocaleString()+' Hz · channel '+s.channel:'No verified matching channel / timing';card.appendChild(note);for(const which of ['context','window']){const button=document.createElement('button');button.textContent='Play '+which;button.disabled=!s||s[which+'_status']!=='complete';button.onclick=()=>play(key,which);card.appendChild(button)}$('players').appendChild(card);const row=document.createElement('tr'),m=s?.metrics;const cells=[names[key],fmt(m?.rms_dbfs),fmt(m?.ac_rms_dbfs),fmt(m?.peak_dbfs),fmt(m?.dc,6),m?.rail_samples??'—',fmt(m?.spectral_centroid_hz,1),fmt(s?.spectrum.resolution_hz,2)];for(const value of cells){const td=document.createElement('td');td.textContent=value;row.appendChild(td)}$('metrics').appendChild(row)}$('settings').textContent=JSON.stringify({settings:selected.settings,notes:selected.notes,recording:D.recording_name,recording_sha256:D.recording_sha256,package_sha256:D.package_sha256,models:D.model_settings},null,2);plot('envelope',series);plot('spectrum',series,true)}
window.addEventListener('resize',()=>{if(selected)draw()});populate();
</script></body></html>'''


def write_page_assets(out: Path, result: dict) -> None:
    """Keep initial HTML small; each selected window loads one local script."""
    windows_dir = out / "windows"
    windows_dir.mkdir(exist_ok=True)
    summaries = []
    for index, item in enumerate(result["windows"]):
        asset = f"windows/{index:04}.js"
        (out / asset).write_text(f"window.SSI_WINDOWS[{safe_json(item['id'])}]={safe_json(item)};\n",
                                encoding="utf-8")
        summaries.append({"id": item["id"], "segment_id": item["segment_id"], "kind": item["kind"],
                          "settings": {"phone_label": item["settings"].get("phone_label", "")},
                          "plot_asset": asset})
    # Complete reports stay in comparison.json; the UI only needs these fields.
    compact = {key: result[key] for key in ("synthetic", "recording_name", "recording_sha256",
                                          "package_sha256", "model_settings")}
    compact["model_flags"] = result.get("model_flags", [])
    compact["analysis"] = {key: value for key, value in result["analysis"].items() if key != "windows"}
    compact["windows"] = summaries
    (out / "index.html").write_text(PAGE.replace("__DATA__", safe_json(compact)), encoding="utf-8")


def build_comparison(package: Path, wav: Path | None, out: Path, *, demo: bool = False,
                     prototype_gain: int = renderer.DEFAULT_PROTOTYPE_GAIN,
                     voice_trim: int = renderer.DEFAULT_VOICE_TRIM,
                     articulation_reference: int = 8) -> dict:
    if demo == (wav is not None):
        raise ValueError("Choose one real WAV or --demo")
    trace = load_calibration(package)  # Checks hashes and every ordered package write.
    manifest = capture.load_manifest(package)
    out.mkdir(parents=True, exist_ok=True)
    model_dir = out / "models"
    source_snapshot = renderer.source_hashes()
    executable = renderer.build_host()
    reports = {profile: renderer.render(trace, model_dir, profile, executable,
        articulation_reference=articulation_reference, prototype_gain=prototype_gain, voice_trim=voice_trim)
        for profile in PROFILES}
    if source_snapshot != renderer.source_hashes():
        raise RuntimeError("Host sources changed during comparison rendering; rerun to keep provenance consistent")
    models = {profile: capture.read_wav(model_dir / f"{profile}.wav") for profile in PROFILES}
    synthetic = None
    recording_path = out / "recording.wav"
    if demo:
        rate, samples, _ = models["baseline"]
        synthetic = synthetic_capture(samples, rate, manifest, recording_path)
        recording_name = "Synthetic baseline + ideal AY markers"
    else:
        assert wav is not None
        recording_name = wav.name
        if wav.resolve() != recording_path.resolve():
            shutil.copyfile(wav, recording_path)
    recorded = capture.read_wav(recording_path)
    analysis = capture.analyze(recorded[1], recorded[0], recorded[2], manifest)
    analysis["input_sha256"] = digest(recording_path)
    (out / "capture_analysis.json").write_text(json.dumps(analysis, indent=2, allow_nan=False) + "\n")
    model_flags = []
    for profile, report in reports.items():
        rails = sum(channel["rail_samples"] for channel in report["statistics"])
        saturation = sum(channel.get("state_saturations", 0) for channel in report.get("native_metrics", []))
        if rails:
            model_flags.append(f"The {profile} model reaches a digital rail in {rails} channel samples; its gain is not normalized.")
        if saturation:
            model_flags.append(f"The {profile} model reports {saturation} internal state saturations; inspect its numerical limits.")
    result = {"schema": 1, "synthetic": synthetic, "recording_name": recording_name,
              "recording_sha256": digest(recording_path), "package_sha256": digest(package),
              "trace_sha256": trace.metadata["trace_sha256"],
              "comparison_source_sha256": digest(Path(__file__)),
              "analyzer_source_sha256": digest(Path(capture.__file__)),
              "analysis": analysis, "models": reports, "model_flags": model_flags,
              "model_settings": {"prototype_gain": prototype_gain, "voice_trim_q16": voice_trim,
                                 "articulation_reference_rate": articulation_reference},
              "windows": compare_windows(manifest, analysis, recorded, models),
              "limits": ["One recording-wide affine map; no per-phone alignment or gain fitting.",
                         "Recorder frequency axes and playback speed remain unchanged.",
                         "Model outputs omit AY, the Phasor analog mixer and recorder response.",
                         "All candidate windows remain unverified; 40 ms FF steps are gross behavior only."]}
    (out / "comparison.json").write_text(json.dumps(result, separators=(",", ":"), allow_nan=False) + "\n")
    write_page_assets(out, result)
    media = out / "media"; media.mkdir(exist_ok=True)
    for key, source in {"capture": recording_path, **{p: model_dir / f"{p}.wav" for p in PROFILES}}.items():
        write_audio_payload(media / f"{key}.js", source, key)
    print(f"{analysis['status']}: {len(result['windows'])} windows -> {out / 'index.html'}")
    if synthetic:
        print(DEMO_LABEL)
    for flag in analysis["flags"]:
        print("REVIEW:", flag)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--wav", type=Path, help="Untrimmed physical tester stereo WAV")
    source.add_argument("--demo", action="store_true", help="Create unmistakably synthetic tooling preview")
    parser.add_argument("--package", required=True, type=Path, help="Checked SSI263-CAL-PAL-01.zip")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ssi263_host/comparison_preview")
    parser.add_argument("--prototype-gain", type=int, default=renderer.DEFAULT_PROTOTYPE_GAIN)
    parser.add_argument("--voice-trim", type=int, default=renderer.DEFAULT_VOICE_TRIM)
    parser.add_argument("--articulation-reference-rate", type=int, default=8)
    args = parser.parse_args()
    try:
        build_comparison(args.package, args.wav, args.output, demo=args.demo,
                         prototype_gain=args.prototype_gain, voice_trim=args.voice_trim,
                         articulation_reference=args.articulation_reference_rate)
    except (ValueError, RuntimeError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
