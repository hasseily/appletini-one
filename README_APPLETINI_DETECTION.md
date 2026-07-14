# Detecting an Appletini from 6502 Assembly

This document describes the supported way for Apple II host software (6502
assembly, or any language that can make a SmartPort call) to determine whether
it is running on a machine that contains an **Appletini** card.

## TL;DR

Issue a **SmartPort `STATUS` call** with **status code `$03`** (GETDIB —
"Return Device Information Block") to **unit `$00`** (the SmartPort *controller*)
in **slot 7**. The returned Device Information Block contains an ASCII ID
string. On an Appletini that string is **`Appletini SP`** (from the controller)
or **`Appletini HD`** (from an individual device unit). Compare the first 9
characters — **`Appletini`** — to identify the Appletini SmartPort firmware.

## Why this is the right mechanism

- **It's a standard, documented protocol call.** The DIB ID-string field exists
  precisely to carry a vendor/device name. No soft-switch probing, no writable-
  ROM tricks, no cross-slot games.
- **The controller (unit 0) answers even with no disk images mounted.**
  See `build_sp_status()` in `ps_sources/frontend/smartport_service.c`:
  ```c
  if (unit == 0x00U) {                 /* Unit 0 = SmartPort controller. Always present. */
      switch (status_code) {
      case SP_STATUS_GETDIB: {
          g_scratch[0] = smartport_present_count();
          for (i = 0; i < 13; ++i) g_scratch[8 + i] = ID_STR_CTRL[i];  /* "Appletini SP" */
          ...
  ```

The ID strings are defined in `ps_sources/frontend/smartport_service.c`:

```c
static const char ID_STR_CTRL[13] = { 12, 'A','p','p','l','e','t','i','n','i',' ','S','P' };
static const char ID_STR_DEV [13] = { 12, 'A','p','p','l','e','t','i','n','i',' ','H','D' };
```

## Important caveat: wait for the boot-menu handoff

Slot 7 presents the SmartPort ROM only **after the Appletini boot menu hands off
to it**. The state machine in `hdl/apple/boot_menu_card.sv`:

- On power-up and after **every Ctrl-Reset**, `slot7_mode_q` reverts to
  `SLOT_MODE_BOOTMENU`. In this state slot 7 is the *boot-menu* card, which
  advertises itself as a Disk II (`$C707` reads `$3C`) and will **not** answer a
  SmartPort GETDIB.
- It flips to `SLOT_MODE_SMARTPORT` only when `handoff_entry_read` fires (the
  host fetching the boot entry point). After that it stays SmartPort until the
  next reset. (Even a Disk II boot leaves SmartPort active in slot 7 — Disk II
  just additionally activates slot 6.)

SuperSprite also requires slot 7. Enabling SuperSprite gates SmartPort off, so
GETDIB detection is unavailable in that configuration.

For **normally-running application code** loaded through SmartPort or Disk II,
the handoff has already happened. To be safe in any context (reset-vector hooks,
etc.), the routine first
verifies the SmartPort ROM signature is actually present in slot 7 and bails out
cleanly if it is not (i.e. "Appletini not yet handed off" reads the same as "no
Appletini").

## Slot-7 SmartPort firmware facts used below

| Address | Value | Meaning |
|---------|-------|---------|
| `$C701` | `$20` | SmartPort/ProDOS signature |
| `$C703` | `$00` | SmartPort/ProDOS signature |
| `$C705` | `$03` | SmartPort/ProDOS signature |
| `$C707` | `$00` | SmartPort device (`$3C` instead = boot-menu phase, *not ready*) |
| `$C7FF` | `$0A` | ProDOS entry offset → ProDOS entry `$C70A` |
| `$C70D` | —     | SmartPort dispatch entry (`= $C70A + 3`) |

(From `hdl/apple/smartport_a2retronet_style_c700.mem`.)

## SmartPort STATUS / GETDIB call convention

The Appletini slot-7 dispatch uses the standard inline-parameter SmartPort
convention: `JSR` the dispatch address, then place the command byte and a
pointer to the parameter list immediately after the `JSR`; the dispatcher skips
them and returns past. Carry clear = success, carry set + A = error.

The STATUS parameter list is:

| Offset | Bytes | Field |
|--------|-------|-------|
| 0 | 1 | parameter count (`$03`) |
| 1 | 1 | unit number (`$00` = controller) |
| 2 | 2 | status-list (result buffer) pointer |
| 4 | 1 | status code (`$03` = GETDIB) |

The **controller** DIB returned at the result buffer:

| Offset | Field |
|--------|-------|
| 0 | number of devices |
| 8 | ID string length (`$0C`) |
| 9 … 20 | ID string text — `"Appletini SP"` |
| 27, 28 | firmware version major, minor |

(A *device* GETDIB — unit `$01`+ — instead puts the length at offset 4 and the
text `"Appletini HD"` at offset 5.)

## Complete sample (ACME syntax)

A ready-to-assemble copy lives at [`software/detect_appletini.a65`](software/detect_appletini.a65).

```asm
; detect_appletini.a65 — detect an Appletini from 6502 assembly.
;
; Method: the Appletini emulates a SmartPort controller hard-wired to slot 7
; that cannot be disabled or relocated. Issue a SmartPort STATUS call with
; status code $03 (GETDIB) to unit 0 (the controller); the returned Device
; Information Block ID string is "Appletini SP".
;
; Returns:  carry CLEAR  -> Appletini present (falls through to `found`)
;           carry SET    -> not present / slot 7 not yet handed off to SmartPort
;
; Assemble:  acme -o detect_appletini detect_appletini.a65

!cpu 6502
* = $0800

SLOT        = 7
ROM         = $C000 + (SLOT * $100)   ; $C700  slot-7 ROM
SIG5        = ROM + $05               ; $C705  -> $03 on a SmartPort card
SIG7        = ROM + $07               ; $C707  -> $00 on a SmartPort card
SP_ENTRY    = ROM + $0D               ; $C70D  SmartPort dispatch (= ProDOS entry + 3)

SETSLOTCXROM = $C006                  ; IIe: make peripheral-card ROM visible in $Cx00

buffer      = $0900                   ; DIB result buffer (>= 32 bytes)
SIG_LEN     = 9                       ; compare just "Appletini"

; ===================================================================
detect
                ; On a IIe, ensure slot ROM (not internal Cx ROM) is mapped so
                ; both the signature read and the dispatch call hit the card.
                sta SETSLOTCXROM        ; write-only soft switch; value irrelevant

                ; --- 1. Is slot 7 currently presenting the SmartPort ROM? ----
                ; During the boot-menu phase $C707 reads $3C, so this guard
                ; cleanly rejects "Appletini present but not yet handed off".
                lda SIG5
                cmp #$03
                bne not_present
                lda SIG7
                bne not_present         ; must be $00

                ; --- 2. SmartPort STATUS / GETDIB to unit 0 (controller) -----
                jsr SP_ENTRY
                !byte $00               ; CMD = STATUS
                !word params
                bcs not_present         ; carry set -> call failed

                ; --- 3. Compare controller ID string to "Appletini" ----------
                ; Controller DIB: buffer+8 = length ($0C), buffer+9.. = text.
                ldx #0
cmp_loop
                lda buffer + 9, x
                cmp sig_text, x
                bne not_present
                inx
                cpx #SIG_LEN
                bne cmp_loop

found
                clc                     ; carry clear = Appletini detected
                rts                     ; <-- your code: it's an Appletini

not_present
                sec                     ; carry set = not detected / not ready
                rts

; ---- SmartPort STATUS parameter list -------------------------------
params
                !byte $03               ; parameter count
                !byte $00               ; unit 0 = SmartPort controller
                !word buffer            ; status-list (result) pointer
                !byte $03               ; status code $03 = GETDIB

sig_text
                !text "Appletini"       ; plain low ASCII, matches firmware
```

### Notes / portability

- **High vs low ASCII.** The firmware emits the name in plain low ASCII
  (`'A' = $41`), so compare against `!text "Appletini"` (which ACME emits as
  `$41 …`). Do **not** set the high bit on the comparison string.
- **Deriving the dispatch address portably.** This sample hard-codes
  `SP_ENTRY = $C70D`, which is correct for the current Appletini firmware
  (`$C7FF = $0A` → ProDOS entry `$C70A`, SmartPort entry `+3`). For maximum
  portability across SmartPort cards, read `$C7FF`, form the ProDOS entry
  `$C700 | ($C7FF)`, and call that address `+3`.
- **Controller vs device.** Using unit 0 is preferred for "is the card here at
  all", because the controller always responds regardless of mounted images.
  If you instead GETDIB a device unit (`$01`+), the text is `"Appletini HD"` and
  starts at `buffer+5`.
- **`SETSLOTCXROM`.** The `sta $C006` line is only meaningful on the //e family
  (it banks in peripheral-card ROM). It is harmless on a II/II+. If your code
  must preserve the prior `INTCXROM`/`SLOTCXROM` state, save it and restore it
  after the call.
