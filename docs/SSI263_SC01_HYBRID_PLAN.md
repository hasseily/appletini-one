# SSI-263 with SC-01 backend plan

## Goal

Keep the tested SC-01 formant filter and sample pipeline, but stop asking its
phoneme ROM and timing rules to stand in for the SSI-263. Both Phasor speech
sockets must act as SSI-263AP devices. The SC-01 code stays as the sound engine,
not as the SSI control source.

This is an incremental path. It does not import the full discrete SC-02 model
or claim exact analog sound.

## Source authority

Use the sources in this order:

1. The SSI-263A data sheet defines registers, CTL, D7/A/R, reset, public clock
   formulas, and DR modes.
2. The supplied `ssi263a.bin` defines all 64 phone rows at address
   `{phone[5:0], selector[2:0]}`. Its source identity is CRC32 `CC0A72EE` and
   SHA-256
   `9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941`.
3. The archived SC-02 prototype schematic may define an inner path only when it
   does not conflict with the data sheet.
4. Real Phasor captures settle sound level and any production detail that the
   prototype cannot prove.

The ROM address, byte layout, and prototype-derived `TPARM` control paths are
recorded in [SSI263_SC02_ROM_FORMAT.md](SSI263_SC02_ROM_FORMAT.md).

Do not add a sound tweak that conflicts with one of the first three sources.
Do not move a chip fault into the Phasor card mix or output stage.

## Current loss in the main implementation

The old SSI path maps 64 SSI codes to 45 SC-01 codes and only 38 distinct
SC-01 target tuples. Twenty valid SSI phones map to SC-01 STOP:

```text
04 06 12 15 17 1E 1F 21 22 2A 2B 2D 2E 31 3A 3B 3C 3D 3E 3F
```

All twenty have a nonzero native voice or fricative target. The native ROM has
64 distinct full rows and 63 distinct upper target tuples. The only equal upper
tuple is HF/HFC; their low control bits differ. A better output gain cannot
recover data that the phone map removed.

The old backend also clears all tract filter history at every phone start,
treats filter frequency `FF` as silence, and lets the SC-01 ROM duration drive
SSI D7. Those rules do not describe an SSI-263.

## Phase 1: safe hybrid base

- Add the exact 512 active ROM bytes as `hdl/apple/ssi263_sc02_rom.mem`.
- Address the ROM with the raw SSI phone and selector. Do not address it with a
  mapped SC-01 phone.
- Feed native F1, F2, F2 resonance, shared F3/F4, voice amplitude, and
  fricative amplitude targets into the current SC-01 update and filter path.
- Keep the SC-01 coefficient functions, multiply-accumulate pipeline, output
  limit, and Votrax support code.
- Use two SSI-263AP bus sockets in Phasor. Do not expose the second socket as a
  Votrax interface.
- Feed both sockets from Apple Q3 through the SSI DIV2 clock enable. AY clock
  doubling in Phasor mode must not change SSI XCK.

For the first pass, convert a native target code to the nearest current SC-01
coefficient index with a named table. Do not hide the conversion in phone
exceptions. This preserves all phone identities while keeping the existing
filter hardware.

## Phase 2: public SSI timing

- Move SSI response timing and D7 authority out of the SC-01 ROM duration.
- Count the effective XCK after DIV2.
- Use `4096 * (16 - R)` effective ticks for a frame.
- Use `(4 - D) * 4096 * (16 - R)` effective ticks for a phoneme.
- Latch the requested DR behavior on CTL falling edge. DR `00` disables A/R and
  keeps the prior response and inflection mode; D7 still records the response.
- Treat completion as a request boundary. Do not clear the audio tract or stop
  filter clocks when D7 changes.
- Keep the current phone and response phase running until a new phone or CTL
  start replaces it. A reg1/reg2 ACK clears D7 without restarting the tract.
- Load a new RATE at the next 1/16-frame counter reload. Do not rewrite the
  response slot in progress.
- Reset the response phase on phone start. A long CTL stop must not cut the
  first phone that follows it.

The bus wrapper may keep its current AppleWin address and IRQ routing. Tests
must cover all `R=0..15`, all DR modes, request acknowledge writes, and a CTL
stop followed by several wait lengths.

## Phase 3: state continuity and filter clock

- Keep formant filter history across phone boundaries.
- Remove the hard mute for filter frequency `FF`. The data-sheet formula is
  `XCK / (2 * (256 - FF))`, so `FF` is the fastest valid setting.
- Mute only for CTL/power-down or host amplitude zero.
- Keep registers through power-down as the AP rules require. Split register
  retention from FPGA configuration reset if the implementation needs a known
  power-up seed.
- Seed transitioned inflection once on the first transitioned start. Do not
  reset it per phone and do not let pitch drift upward across a phrase.

## Phase 4: native source and route detail

This phase can still use the SC-01 filter pipeline.

- Replace the SSI-only SC-01 phone-class noise gates with the native ROM control
  sequence.
- Use the two held fricative routes from U20B, U112, and U166. They are not one
  live ROM bit and its complement.
- Gate the existing noise-before-F3 and noise-after-F3 insertion points with the
  two native routes.
- Use the native U75/CD4006 recurrence for SSI noise. Keep the current SC-01
  noise source for the Votrax path.
- Let selector 3 control both F3 and F4 on the SSI path.
- Scale SSI formant choices with filter frequency instead of adding
  phone-specific presence, burst, or closure rules.

## Prototype facts that stay deferred

Do not promote these to production behavior without more proof:

- the final P2/R301 fitted state and its U96 RATE=F effect;
- prototype transition rates that make articulation or pitch movement depend
  on speech rate when the data sheet says they do not;
- the free-running U37 phase during CTL stop;
- the unreset U65/U64 power-up value;
- POT3, C381, card amplifier, and absolute voice/noise level;
- any discrete-board change that may not exist in the production die.

The safe policy for CTL is a full next response interval. The safe first pitch
seed is the current target once per die lifetime. Both choices need real-card
capture before they become claims about internal silicon.

## SC-01 backend ceiling

This hybrid can fix missing phones, bad request timing, wrong state resets,
filter-frequency handling, and much of the source routing. It cannot prove the
exact SSI transfer curves while it still uses:

- SC-01 filter coefficients and bandwidths;
- the SC-01 filter order where it differs from the SC-02 tract;
- SC-01 excitation levels or output shaping;
- hand-converted native target codes;
- a digital output stage with no measured SSI/Phasor analog reference.

Call a good result "SSI control with an SC-01 formant backend," not a full
SC-02 replica. Cross that ceiling only after same-vector output captures from a
real SSI-263 Phasor.

## Required checks

Run the fast contract test on every step:

```powershell
python scripts\test_ssi263_sc01_hybrid.py
```

It checks the full supplied-ROM hash reconstructed from the active table, all
64 rows and addresses, the twenty phones formerly lost to STOP, two SSI
sockets, XCK wiring, filter state continuity, valid `FF` handling, source-list
closure, and a CTL stop/wait/start timing reference.

Add RTL simulation beside it for:

- all 64 phones reaching distinct native rows;
- every `R` and DR duration boundary;
- immediate and transitioned pitch without phrase-to-phrase drift;
- D7/A/R set, read, clear, and mode-switch routing;
- filter history before and after a phone boundary;
- `AH S AH`, `AH P AH`, and `HF EH1 L O` vectors.

Those phrase checks catch regressions. They do not calibrate the chip. Use a
real-card capture for that step.
