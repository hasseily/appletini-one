import collections
import csv
import hashlib
import json
import struct
import time
import profile_harness as h

c = h.connect()
try:
    h.paused(c)
    c.bp_clear_all()
    expected = (h.ROOT.parents[1]/'docs/Apple2e_Enhanced.rom').read_bytes()[0x1000:]
    actual = c.read_mem(0, 0xd000, 0x3000)
    assert actual == expected, 'Emulator Applesoft/monitor ROM mismatch'
    start = h.registers(c)
    h.control('START', 'VERIFY_TRACE')
    c.stops.clear()
    c.step_into(1000)
    deadline = time.monotonic()+10
    while not c.stops and time.monotonic()<deadline:
        c.get_status()
        time.sleep(.02)
    assert c.stops, 'No step-complete event'
    end = h.registers(c)
    h.control('STOP', 'VERIFY_TRACE')
    trace = c.get_trace(count=1000).entries
    counters = collections.defaultdict(lambda: [0,0])
    for i, entry in enumerate(trace):
        cycle = struct.unpack_from('<Q', entry)[0]
        pc = struct.unpack_from('<H', entry, 16)[0]
        next_cycle = struct.unpack_from('<Q',trace[i+1])[0] if i+1<len(trace) else end['cycle']
        assert not (struct.unpack_from('<H',entry,36)[0]&1), 'Unexpected IRQ in validation'
        counters[pc][0] += 1
        counters[pc][1] += next_cycle-cycle
    with (h.OUT/'VERIFY_TRACE.csv').open(newline='') as f:
        measured = {int(r['pc']):[int(r['instructions']),int(r['cycles'])] for r in csv.DictReader(f)}
    assert dict(counters) == measured, 'Profiler differs from independent trace histogram'
    meta = json.loads((h.OUT/'VERIFY_TRACE.json').read_text())
    assert meta['instructions'] == 1000
    assert meta['accounted_cycles'] == end['cycle']-start['cycle']
    result = dict(rom_d000_ffff_sha256=hashlib.sha256(actual).hexdigest(),
                  mapped_rom_matches=True, trace_records=1000, distinct_pcs=len(counters),
                  cycles=meta['accounted_cycles'], independent_trace_histogram_matches=True,
                  start=start, end=end)
    (h.ROOT/'capture-validation.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result))
finally:
    c.close()
