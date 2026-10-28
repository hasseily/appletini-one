# VIA calibration and STARFALL IRQ check

The PAL ONE//e VIA clock is about **1.0178 MHz**, so use **0.9825 microseconds
per tick** for this board profile. The timer keeps that rate in the 1 MHz,
26 MHz and TURBO CPU presets.

The raw FIFO path preserved STARFALL's audio clock with 128-, 511- and
512-byte replies at the 26 MHz preset and TURBO. The 512-byte ROM path
lost ticks at the 26 MHz preset. Keep STARFALL on the RAM FIFO driver and
its planned 128-byte transfer area.

## Calibration

The test compared VIA-A T2 with the ARM global timer. The ARM reported
333333343 counts per second. Each sample brackets a 64-bit ARM timestamp
with two VIA samples. The interval between timestamps covers about eight
seconds of continuous VIA sampling.

| CPU preset | VIA clock bounds, Hz |
| --- | ---: |
| 1 MHz, timer-only loop | 1017629.22–1018063.77 |
| TURBO | 1017674.41–1018015.81 |
| 26 MHz menu preset, divider 5 | 1017683.14–1017993.47 |

All intervals contain the RTL cadence: 133333333.33 fabric clocks/second
divided by 131, or about 1017811.70 VIA ticks/second. The common interval
supports the rounded conversion of 0.9825 microseconds per tick. The
bounds include sample granularity and request/reply time. This compares
two SoC clocks; it is not an external frequency-counter measurement.

STARFALL currently passes `via_hz=0`, which makes libtini assume 1000000
Hz and program a 10000-tick audio period. On this PAL profile that is about
101.8 interrupts/second before losses. For a nominal 100 Hz clock, use:

```c
config.via_hz = 1017812UL;
/* libtini computes latch 10176, period 10178 device ticks. */
```

The test programmed that latch at runtime. It did not change STARFALL's
release sources or set a global constant for other video/host profiles.
Phasor native low-counter reads can add a device clock. The reference
probe reads only T2 high, which avoids that effect and does not acknowledge
STARFALL's T1 interrupt.

## Audio workload

The physical board ran the actual copied STARFALL program with Phasor
audio, music, prepared speech, game simulation and graphics. The harness
restarted sector 1 before each short phase. It ran the actual stock C800
ROM or the same RAM FIFO copy loop used by the transport checkpoint.
Replies came from the private deterministic test source. No HTTP, TLS,
JSON parsing or leaderboard service ran in this test.
All normal calls checked return status and bounds; the final full reply
of each phase also matched the independent data pattern.

The table gives observed maximum IRQ-masked intervals. These use an
independent timer with 256-tick granularity, about 0.252 ms. Allow about
0.28 ms either side for granularity and the probe's entry/exit code.
These are measurements from this workload, not hard deadlines.

| Path | TURBO, ms | 26 MHz preset, ms | Audio tick result |
| --- | ---: | ---: | --- |
| Raw FIFO, 128 bytes | 1.76 | 2.01 | No loss detected at either speed |
| Raw FIFO, 511 bytes | 3.02 | 3.52 | No loss detected at either speed |
| Raw FIFO, 512 bytes | 3.02 | 4.02 | No loss detected at either speed |
| ROM, 512 bytes | 9.56 | 10.82 | No loss detected in TURBO; loss at 26 MHz |
| Deliberate 25 ms block | 25.15 | 25.15 | Large loss at both speeds |

Each read phase made 129–141 calls in about seven seconds. The 26 MHz
ROM phase counted 705 audio ticks against about 707.46 elapsed periods.
The difference exceeds the one-tick phase uncertainty: at least one tick
was lost, with about two indicated. Its largest IRQ service gap was
20.12 ms. The raw 512-byte IRQ gaps reached 12.58 ms in TURBO and 13.08 ms
at the 26 MHz preset, but the next interrupt caught up without a lost
timer expiry. A delayed interrupt is not always a lost tick.

The deliberate long block counted 567 ticks against about 705.32 periods
in TURBO, and 564 against about 706.18 periods at the 26 MHz preset.
This confirms that the probe detects timer coalescing and clock slowdown.
The probe's timer-IRQ count matched the original driver's tick count in
every phase. ARM timestamp checks also confirmed the extended timebase
across every phase.

All accepted phases ended in active play with no `CLOCK GAP` flag. The
user heard normal output before the earlier low-speed run, then reported
slowing and `CLOCK GAP` during that run. This was not a recorded waveform
or a full listening test of every accepted phase.

## Frame rate and limits

In TURBO, baseline and normal reads stayed about 30 rendered frames/second.
The 26 MHz preset's instrumented baseline was 21.65 frames/second; raw
128-byte reads gave 20.35, raw 511/512 gave about 19.5, and ROM reads 15.69.
The probe adds timer I/O on foreground audio callbacks, which also invokes
the configured slot-4 slowdown. These are instrumented rates, not stock
STARFALL performance. They do not establish 30 fps acceptance at 26 MHz.
A final API integration still needs a low-overhead frame-rate and listening
check, especially near the game's minimum usable CPU speed.

The full-game 1 MHz attempt is rejected. STARFALL was not playable and
showed `CLOCK GAP` even in the no-network baseline. Long stalls also made
the first IRQ-assisted timer extension miss wraps. Its apparent 842 kHz
rate was invalid. The later continuous timer-only loop fixed the sampling
method and confirmed the real clock at 1 MHz. No audio/game acceptance
is claimed at that CPU speed.

The earlier native transport measurements can now be converted: the
11313-tick raw maximum, at 511 bytes, is about 11.115 ms; the 512-byte
maximum of 10820 ticks is about 10.631 ms; the 128-byte maximum of 3793
ticks is about 3.727 ms. Thus a full native-size read can exceed a 100 Hz
audio period even though STARFALL itself does not target that speed.

## Implementation and restoration

The opt-in ARM build adds only a private timestamp request under
`APPLETINI_TIMER_CALIBRATION`. Normal descriptor requests stay unchanged.
The FPGA, ROMs and Golden payload were not rebuilt or changed.

The runtime Apple probe lives in a checked sector-1 overlay gap, with
separate data and reply space below the scene. It preserves the original
audio handler and its T1 acknowledgement. It uses direct, checked CPU
pause/resume for loading. It freezes results before reading the payload;
a live dump can otherwise catch the last byte at its guard initializer.
Boss overlays are excluded from this diagnostic's short restarted runs.

Host checks cover the actual probe, ROM/raw copies, C8 restoration, guard
bytes, original-driver IRQ acknowledgement, frozen payload integrity and
the tight timer-extension loop. The ARM packet test checks 64-bit time,
frequency, bounds and the unchanged normal descriptor. ARM builds and the
frontend memory audit passed.

The exact stock F1.3.3b3 image and saved settings are restored by SD
readback. The original STARFALL mount is open, the owned `/IRQCHECK`
directory is removed, and FTP is off. The local normal ARM build also
has both diagnostic flags disabled. No libtini source changed.

Evidence is in `validation/starfall-irq-clock-20261028/`: three clock
calibrations, all phase records, UART output, build/source hashes, host
checks and restoration proof. Earlier failed or rejected harness runs
remain in ignored build output and do not count as acceptance.
