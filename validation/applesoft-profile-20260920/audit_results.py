"""Read-only audit of captures, unchanged inputs, and restored GSSquared files."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
gs = Path(r'C:\Users\hasse\source\repos\gssquared')
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
original = json.loads((root/'runtime/original-sha256.json').read_text())
restored = {name: sha(gs/name) == digest for name,digest in original.items()}
assert all(restored.values()), restored
source = Path(r'C:\Users\hasse\Downloads\short-programs.dsk')
disk_sha = 'a886567d9400af183cf85dbfca84a95e5ec25ad18d2e4036ce725c95a694fbe6'
assert sha(source) == disk_sha
assert sha(root/'runtime/working-short-programs.dsk') == disk_sha
data = json.loads((root/'analysis/profiles.json').read_text())
assert len(data['captures']) == 62
assert len(data['programs']) == 28
assert len(data['failed_captures']) == 1
for capture in data['captures']:
    meta = json.loads((root/'captures'/(capture['runid']+'.json')).read_text())
    assert meta['clock_mode_at_start'] == 0
    assert meta['instructions'] == meta['histogram_instructions']
    assert meta['instruction_cycles'] == meta['histogram_cycles']
    assert meta['accounted_cycles'] == meta['guest_cycle_span']
    assert capture['chrget_snapshot_guard']['stock_at_both_snapshots']
    assert sum(x['cycles'] for x in capture['category_shares']) == meta['instruction_cycles']
geometry = next(x for x in data['captures'] if x['runid']=='GEOMETRY')
duplicate = next(x for x in data['captures'] if x['runid']=='GEOMETRIC')
assert geometry['instructions'] == duplicate['instructions']
assert geometry['instruction_cycles'] == duplicate['instruction_cycles']
good = [x for x in data['captures'] if x['include_in_priority_aggregates']]
result = dict(passed=True, captures=len(data['captures']), useful_captures=len(good),
    successful_program_entries=len(data['programs']), failed_captures=len(data['failed_captures']),
    normal_end_program_entries=len({x['program'] for x in good if x['finished_at_prompt']}),
    useful_instructions=sum(x['instructions'] for x in good),
    useful_cycles=sum(x['accounted_cycles'] for x in good),
    disk_sha256=disk_sha, original_and_working_disk_unchanged=True,
    restored_gssquared_files=restored, all_capture_clock_modes_zero=True,
    all_histograms_match_guest_cycle_spans=True, duplicate_program_counts_match=True)
(root/'final-validation.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
