# Code Review: main, last 3 commits (2026-08-20)

**Scope.** The three most recent commits on `main` (`git diff HEAD~3..HEAD` at the time of review):

- `1f3994a` Add exclusive SD card FTP sharing (#5)
- `c3c75be` Add standalone ONE//e mode (#3)
- `65e66ff` Implement linear text overlay (#2)

**Method.** A multi-agent review at high effort. Ten finder agents produced 56 candidate findings. After deduplication, ten verifier agents examined all correctness candidates against the repository, one verdict each. Tally: **13 CONFIRMED, 3 PLAUSIBLE, 13 REFUTED** (refuted with in-repo proof). Sixteen correctness findings survive. The report cap kept the 10 most severe; this document also contains the 6 findings above the cap, the cleanup candidates that were not adversarially verified, and the refuted candidates.

**How to read this document.** Part 1 contains the 16 verified correctness findings, in severity order. Each finding gives the file and line, the verdict, the defect, and a concrete failure scenario. Part 2 contains 24 cleanup, reuse, efficiency, altitude, and conventions candidates. These come from the finder pass; they did not go through adversarial verification. Part 3 lists the refuted candidates for the record, so that a later review does not resurrect them.

---

## Part 1 — Verified correctness findings (16)

### Findings 1–10 (reported)

#### 1. Stale overlay read byte can drive all bus reads — CONFIRMED
`hdl/apple/smartport_card.sv:455`

An overlay DEVSEL read reply latched into `ab_write_q` (`wr_data_en=1`) is only cleared by the GATED bus's `data_en` or `!res`, but `gate_ab` zeroes `data_en` while leaving `res=1`, so a mid-cycle drop of `vtw_smartport_visible` leaves the stale byte driven onto every subsequent bus read via the unconditionally merged arbiter input.

*Failure scenario:* Slot-7 visibility drops between `serve_en` (~clock 73) and `data_en` (~clock 124) — PS toggles SuperSprite, cold slot scan hides slot 7, or `vtw_disk2_boot_scan_q` rises (visibility inputs are unsynchronized to the window, `apple_top.sv:1506-1510`). The hold never clears (`smartport_card.sv:406-414`, 456-457 test only the gated `ab_read`), `smartport_ab_write` merges into the arbiter every cycle (`apple_top.sv:2002`, `assert_inh` forced 0), and the stale overlay byte corrupts all bus reads — machine crash — until slot-7 service resumes or Apple RESET.

#### 2. ONE//e persist writes SD while FTP owns the volume — CONFIRMED
`ps_sources/frontend/config_menu.c:6840`

`config_menu_poll_onee_mode()` runs every main-loop iteration with no `ethernet_ftp_sd_remote_active` gate, so its persist-retry path writes APPLETINI.CFG to the SD volume the FTP service exclusively owns, and its failure fallback force-remounts a second FATFS work area over the FTP-owned volume.

*Failure scenario:* A pending or retrying ONE//e persist write fires during an exclusive FTP session (`main.c:3527` runs before the modal branch at 3531 every iteration). The save opens '0:' files while `g_ftp.fs` holds open FIL/DIR objects; on any open failure `config_menu_open_path` calls `config_menu_mount_sd()` (`config_menu.c:1272-1283`), remounting `g_config_fs` over '0:/' and leaving the FTP work area stale — two FATFS instances caching FAT sectors for one volume, the classic FatFs corruption pattern, on the card mid-STOR.

#### 3. Warm reset re-hides slot 7 for the rest of the session — CONFIRMED
`hdl/apple/onee_cold_slot_scan.sv:46`

Any virtual RESET (including the user-reachable 8-cycle warm reset pulse) re-executes the cold-boot branch and re-hides slot 7 when the ONE//e boot target is Disk II, and only a $C6xx read ever un-hides it — which a //e warm reset with a valid power-up byte never performs.

*Failure scenario:* In a Disk II ONE//e session the user issues a warm reset via the input bridge. `slot7_hidden` reloads to 1 (lines 41-48); the //e ROM vectors through $03F2 without a slot ROM scan, so no $C6xx read occurs. Slot 7 (SmartPort card, overlay, NSC/SuperSprite via `onee_slot7_cards_visible`, `apple_top.sv:596`) stays hidden for the rest of the session: $C7xx fetches return floating bus and all SD storage vanishes after a mere warm reset.

#### 4. Refused ONE//e start still persists ON to config — CONFIRMED
`ps_sources/frontend/config_menu.c:5036`

`config_menu_toggle_onee_mode` persists ONE//e=ON to the SD config before `onee_service_request_start()`, and only the pl_ready+hazard refusal queues a rollback — every other refusal leaves persisted=ON on disk while the UI shows LOCKED, arming a surprise auto-start.

*Failure scenario:* User selects ONE//e while the bus is not quiet or RESELECT is not armed: the request is refused (`onee_service.c:59-76` falls through to `disarm(1)` with no `force_persisted(0)`), the UI prints LOCKED, but 'onee.standalone.persisted=ON' is already saved (5036-5043 precede the request at 5045). On the next power-up, `restore_persisted` arms `restore_pending` and the machine self-starts ONE//e — isolating the physical Apple bus — with no fresh operator action.

#### 5. Resting gamepad trigger blocks ONE//e unpause forever — CONFIRMED
`ps_sources/frontend/usb_hid_service.c:2155`

`usb_hid_service_all_input_released()` includes `raw_axis_active_mask`, which flags any absolute axis outside the middle third of its logical range and only updates on new reports, so a gamepad trigger resting at logical_min keeps it set forever and main.c's ONE//e unpause gate never fires.

*Failure scenario:* A DualShock-style pad (L2/R2 on RX/RY resting at logical_min, `usb_hid_service.c:1374-1379`, 1474-1479) is connected. User opens the menu during a ONE//e session (core paused), then closes it: `main.c:2778-2785` gates unpause solely on `all_input_released()` with no timeout or fallback, so the 65C02 stays frozen with PAUSE asserted and USB input blocked until the device is unplugged.

#### 6. Rogue PASV connect aborts STOR and leaves the file truncated — CONFIRMED
`ps_sources/frontend/ftp_sd_service.c:962`

The fixed PASV data port (50000) accepts the first on-subnet connection; a wrong peer triggers `ftp_abort_transfer()`, which closes the data socket without re-listening — locking out the legitimate client after STOR already truncated the target file with FA_CREATE_ALWAYS.

*Failure scenario:* Client sends PASV then STOR existing.file: `ftp_prepare_transfer` truncates the file (line 613) while the data socket listens. Any other host on the subnet connects to port 50000 first (W5100 has no SYN-time peer filter); memcmp vs `control_peer` fails, `ftp_abort_transfer` closes the listener (438-439), the real client's data connect is refused, and the existing file is left at zero bytes.

#### 7. Transient fifo_full falsely marks the overlay STALE — CONFIRMED
`hdl/apple/apple_cycle_capture.sv:312`

`overlay_drop_source_q` is a full-native-cycle level (`overlay_rule_valid` lacks the `ab_read.data_en` qualifier that `apple_push_request` has), and the overlay drop sticky requires no `push_request` — so a transient `fifo_full` at any fabric clock in the ~130-clock window sets STALE even when the overlay record was pushed successfully.

*Failure scenario:* Overlay armed under heavy capture traffic (SHR frames): the overlay write pushes fine at `data_en`, but `fifo_full` pulses at another clock in the window — including full caused by the successful push itself — and the sticky (lines 310-313) latches. `linear_text_overlay_card.sv:427-436` then sets STALE, clears armed, and issues FRAME_HIDE: spurious overlay blanking and forced re-ARM with zero data lost. Fix shape: qualify with an actual suppressed push (`push_request && fifo_full`), mirroring the general sticky at line 292.

#### 8. ARM accepted while SHOW pending tears the overlay frame — CONFIRMED
`hdl/apple/linear_text_overlay_card.sv:312`

CMD_ARM is accepted while a FRAME_SHOW is still pending (line 305 gates only on `busy_q`, not `frame_pending_q`), so the new ARM overwrites `armed_*` that the pending SHOW's later PS ACK copies into `active_*`, while CPU1 blank-fills the very slot the SHOW is about to display.

*Failure scenario:* The 6502 issues CMD_SHOW then CMD_ARM before CPU0 acks the SHOW at a frame edge. CPU1 picks the fill slot as the opposite of the not-yet-flipped ACTIVE_SLOT (`linear_text_overlay_capture.c:52`) — the slot the pending SHOW will promote — and starts clearing it; the SHOW then latches the new ARM's geometry. The viewer sees a torn or blank overlay frame with the wrong configuration.

#### 9. CLOSE_WAIT aborts RETR/LIST despite pending TX data — CONFIRMED
`ps_sources/frontend/ftp_sd_service.c:1138`

`ftp_data_poll` unconditionally treats W5100_SR_CLOSE_WAIT as failure for RETR/LIST transfers (`ftp_finish_transfer(0)`) even though the W5100 can still transmit in CLOSE_WAIT — the STOR path itself treats CLOSE_WAIT as normal — so clients that half-close the data connection get truncated downloads and a 426.

*Failure scenario:* An FTP client shuts down the write side of its data connection right after connecting (it only reads during RETR). The socket enters CLOSE_WAIT while transmit still works; lines 1138-1139 abort the transfer before `ftp_send_transfer_poll` runs, without checking whether unsent data remains. Reproducible failed/mid-file-truncated downloads; even a FIN arriving after all bytes were queued converts a complete transfer into a 426.

#### 10. Async safety-guard clear paths are unconstrained and can corrupt state — PLAUSIBLE
`hdl/apple/onee_mode_safety_guard.sv:142`

The sticky lockout and run-kill flops use multi-input combinational cones (`|(raw^sync)` of six async Apple pins; mode-kill OR) as asynchronous preset/clear, and the asynchronously-cleared `onee_run_q` fans combinationally into the apple_top `ab_read` mux, soft_switch_manager, arbiter `inh_allowed`, vTW enable, and audio gating with no resynchronizer and no recovery/removal constraint in `appletini_yarz.xdc`.

*Failure scenario:* A LUT glitch on the async preset (several sync-vector bits updating on one edge) spuriously kills a running ONE//e session; worse, a genuine async clear of `onee_run_q` landing inside the setup window of its 133 MHz consumers lets different flops sample a mix of virtual and physical bus fields on the same edge, corrupting SoftSwitchState and card state that persists into the next physical-host session. The xdc covers only pad max-delays and reset_sync CLR pins (lines 577, 673) — nothing constrains these paths.

### Findings 11–16 (above the 10-slot cap)

#### 11. ONE//e keyboard mirrors $C001-$C00F and $C01x low bits return floating data — CONFIRMED
`hdl/apple/onee_motherboard_io.sv:123`

Keyboard read decode claims only $C000 and $C010; reads of mirrors $C001-$C00F fall to default with `read_claim=0` and return floating/scanner data, and $C011-$C01F status reads return `floating_bus_data[6:0]` instead of the key code, unlike a real Enhanced //e.

*Failure scenario:* Software on ONE//e that polls the keyboard through a mirror address (LDA $C001..$C00F — used by some games and protected titles) never sees bit 7 or the key code, so keyboard input is dead for that program; code that masks bits 6:0 of a $C01x status read expecting the last key code gets floating bytes (`apple_virtual_bus.sv:93-98` serves `floating_bus_data` for unclaimed reads).

#### 12. Boot-device guard removal lets a physical host silently boot the wrong device — CONFIRMED
`ps_sources/frontend/config_menu.c:2611`

The diff removed `config_menu_coerce_boot_device()` and both 'ENABLE DISK II TO BOOT SLOT 6' guards; boot handoff now publishes CONFIG_BOOT_HANDOFF_DISK2 even when slot 6 is disabled, relying on a silent PL fallback to SmartPort (`boot_menu_card.sv` `handoff_disk2 = disk2_enabled && handoff`), with no replacement UI warning.

*Failure scenario:* On a physical host with slot 6 disabled, the user or a loaded profile sets boot device = Disk II; the menu accepts, persists, and displays 'Boot device: Disk II' (`config_menu.c:5232-5241`, `config_menu_main_tabs.c:123-129` have no slot-6 cross-check) while the machine actually boots SmartPort — persisted state permanently disagrees with real boot behavior with no message. The removal was needed for ONE//e's always-present virtual Disk II, but the physical-host mismatch is unmitigated.

#### 13. Double list2cmdline quoting breaks the timing-firmware packager on spaced paths — CONFIRMED
`scripts/package_timing_firmware.py:104`

`run_batch` builds `'call ' + list2cmdline(args)` as one element of `['cmd.exe','/d','/c', command]`; subprocess applies list2cmdline again over the outer list, escaping the inner quotes as `\"` — a convention cmd.exe does not parse — so any path containing a space breaks or mis-splits.

*Failure scenario:* Repo or Vitis install under a spaced path (e.g. 'C:\Program Files\...'): both call sites (line 145 vitis workspace build, lines 156-160 `make_firmware_bin.bat` with four artifact paths) produce a mangled command line; the batch call runs a wrong path or passes truncated arguments, and `make_firmware_bin.bat` can fall back to default stale ./project bitstream paths — packaging firmware from the wrong artifacts. Latent today only because current paths are space-free.

#### 14. Render harness reads past the buffer for truncated assets — CONFIRMED
`scripts/host_render_harness/harness.c:803`

`t6_dhgri_static_cache` (line 803) and `t8_video7_mix_load_hold` (line 948) read `f[0x607C]` before the `expect(len == 0x8000)` check; `expect()` only counts failures without aborting, and `load_file` mallocs exactly the file length, so a truncated asset causes heap OOB reads.

*Failure scenario:* `software/legacy_demo_images/face.dhri` truncated below 0x607D (partial checkout, LFS placeholder, regenerated asset): `f[0x607C]` is a heap out-of-bounds read, then the feed loops index up to `f[0x7FFF]` regardless of len (t6 lines 818-824; t8's bank calls at 1026+), smashing far past the allocation — the harness crashes or reports garbage pixel diffs instead of a clean length-failure message. Test-harness-only severity.

#### 15. TEXTOVERLAY slot probe can latch a spinning Disk II into write mode — PLAUSIBLE
`software/textoverlay.a65:147`

`find_overlay`'s slot probe reads DEVSEL +$0E first (comparing against $4C) and touches +$0F only on a match — but on a Disk II controller +$0E is Q7L returning the live shift register while the motor spins, so a transient $4C (~1/256 per boot) passes the check and the follow-on $C0nF read (Q7H) latches WRITE mode.

*Failure scenario:* TEXTOVERLAY is BRUN from the slot-6 floppy; within the ~1 s motor spin-down the probe reaches slot 6, Q7L returns a transient $4C, the code reads $C0EF and latches the sequencer into write mode with the disk still spinning under the head, writing garbage onto the boot disk. Low probability per boot, real data-loss consequence; no slot-skip for Disk II signatures exists.

#### 16. Same-second timing builds sort nondeterministically and block promotion — PLAUSIBLE
`scripts/timing_run_helpers.tcl:481`

`require_latest_full_pair` sorts full builds with `lsort -dictionary -index 0` on `utc_start` only (1-second resolution; build_id is not a tiebreaker), so same-second builds tie in filesystem-arbitrary order; `promote_timing_candidate.tcl` line 46 additionally rejects any confirm build whose `utc_start` is not strictly after the tested build's.

*Failure scenario:* Two full builds started in the same second (the -NN build-id scheme explicitly anticipates this): 'the latest two' is nondeterministic under ties, so the pair gate can pass or fail arbitrarily when a third same-second sibling exists, and a fast scripted tested/confirm pair in one second is always refused promotion with a confusing error. Mostly fail-closed; low severity.

---

## Part 2 — Cleanup candidates (24, not adversarially verified)

These candidates come from the finder pass. The verifier pass did not examine them, because correctness candidates filled all verification slots. Treat each one as a probable, not proven, improvement.

### Reuse / duplication

#### R1. FTP service re-implements the W5100 driver layer
`ps_sources/frontend/ftp_sd_service.c:119`

Re-implements the W5100 low-level layer that `uthernet2_control.c` already contains: `w5100_read16/read16_stable/write16` (byte-identical to `uthernet2_control.c:243-282`), ring read/write wrap loops, the Sn_CR command-poll loop, and a second copy of the W5100 register/command/IR constants including the RMSR/TMSR 4+2+1+1 socket map (ftp adds S1 base 0x5000/mask 0x07FF; uthernet2 assumes S0 base 0x4000/mask 0x0FFF). Should be exported from `uthernet2_control.h`, which already exports read/write_block.

*Risk:* Two parallel drivers for one chip diverge: the command-poll loops already disagree (ftp: 1000 tight iterations no sleep; uthernet2: 100 iterations with usleep(1000)). A socket-memory-split change made in one file silently makes the other read/write overlapping FIFO memory; an FSR/RSR or ring-wrap fix in one copy leaves the other broken.

#### R2. $C01x status-read table duplicated between ONE//e and vTW
`hdl/apple/onee_motherboard_io.sv:131`

The $C011-$C01F soft-switch status-read table duplicates the decode in `vtw_core_top.sv:539-559` (disabled under virtual_motherboard so this copy serves instead). The copies already diverge: vTW returns `{bit, 7'b0}`, ONE//e returns `{bit, floating_bus[6:0]}`; their $C019 VBL sources also differ (rewind-corrected vs raw `line_in_frame >= 192`, itself a third copy of that expression). Should be one shared function/module over sss + vbl + floating bits.

*Risk:* The next $C01x/$C019 fix (this table has needed corrective passes before — the vTW Enhanced-//e synthesis and RDVBLBAR work) lands in one copy only; ONE//e and vTW then report different MMU/VBL state for the same software, producing raster glitches and diagnostics divergence that takes a full bus-trace investigation to re-diagnose.

#### R3. Overlay ARM-register decoder defined twice within one feature
`ps_sources/frontend/linear_text_overlay_capture.c:18`

`shadow_slot()` and the ARM0/ARM1/ARM2 bit-field decoder are defined twice within one feature (`linear_text_overlay_capture.c:11-34` and `linear_text_overlay.c:40-65`); both files already share `linear_text_overlay.h` where the decoder belongs once.

*Risk:* Any repacking of the ARM registers (origin_y already straddles ARM1/ARM2 with a non-obvious byte splice) must be re-derived in two places; the copies also derive the slot bit from different state fields (ARMED_SLOT vs inverted ACTIVE_SLOT) — an intentional asymmetry that looks like a bug and invites a breaking 'consistency fix'.

#### R4. RGB565 packing re-implemented instead of FB16_RGB
`ps_sources/frontend/linear_text_overlay.c:12`

RGB565_CONST re-implements FB16_RGB from `ps_sources/lib/fb16.h:38`, the repo's single RGB565 packing definition documented as matching the DVI pins bit-for-bit; the VGA palette table should be built with FB16_RGB.

*Risk:* If the fb16 packing changes, every fb16 consumer follows automatically but the overlay palette keeps the old packing and renders wrong colors — visible only on hardware, in one feature.

#### R5. ~15 new copies of the xvlog/xelab/xsim launcher pair
`scripts/test_apple_virtual_bus.py:18`

This diff adds ~15 new copies of the `vivado_tool()/run()` xvlog-xelab-xsim launcher pair across new test scripts, more than doubling the pre-existing ~10 copies, instead of extracting a shared module (`scripts/` already supports shared imports — `pcpi_disk.py` is imported by the disk build scripts).

*Risk:* The xsim launch/log policy lives in ~25 files and is already drifting (different .bat resolution and failure text between new scripts); a launcher fix (Vivado version change, timeout, log capture on abnormal exit) will in practice reach only actively-touched scripts.

#### R6. Manifest grammar implemented independently in Python and Tcl
`scripts/package_timing_firmware.py:21`

`read_manifest/write_manifest/sha256_file/require_value` re-implement in Python the manifest grammar and checks this same diff defines in `scripts/timing_run_helpers.tcl` (`timing_run::read_manifest/write_manifest/sha256_file/require_manifest_value`) — two independent parser/serializer implementations of one format with no shared spec or round-trip test.

*Risk:* A format change made in the Tcl side (key rename, escaping rule, required field) silently breaks or bypasses the Python `validate_build_manifest`: the firmware packaging gate can pass a manifest the Tcl signoff chain would reject, undermining the hash-chained promotion workflow.

### Simplification

#### S1. FTP modal loop clones the usb0 modal loop in main.c
`ps_sources/frontend/main.c:3531`

The ~130-line FTP-sharing modal loop (3531-3663) is a near-verbatim clone of the usb0_sd_remote modal loop below it (3665-3809): same `was_active/redraw_pending/last_input_seq` trio, budget-8 loops, dual-UART poll, vblank-gated redraw. Differences: active-predicate, service poll function, eject check. One modal-loop helper taking two function pointers would replace both.

*Risk:* Fixes to the modal event plumbing must be applied twice in one file; the copies have already drifted (the FTP redraw block omits the usb0 `needs_attention` condition), and a third exclusive mode adds a third 130-line copy.

#### S2. Three duplications inside ftp_sd_service.c
`ps_sources/frontend/ftp_sd_service.c:1013`

Three duplications in one file: the send-start block (`sent<0 -> finish(0); sent>0 -> buffer_len=0`) is copy-pasted four times (1017-1028, 1045-1055, 1082-1092, control variant 829-840); `ftp_abort_transfer` and `ftp_finish_transfer` repeat an identical 8-line data-session reset (439-448 vs 459-468); `transfer_eof` is dead state — both setters (1040, 1069) call `ftp_finish_transfer` immediately, which zeroes it, so the check at 1013 can never fire.

*Risk:* A partial-send fix gets applied to only some of the four copies; a field added to the session reset drifts between abort and finish, leaking state into the next transfer; the dead flag misleads readers into designing around an 'EOF pending flush' state that cannot occur.

#### S3. Override snapshot-and-rollback copy-pasted in four vtw_service functions
`ps_sources/frontend/vtw_service.c:559`

The snapshot-and-rollback of the override triple (`old_ovr_active/mode/div` saved, mutated, restored on `vtw_override_apply` failure) is copy-pasted in four functions (559-575, 590-615, 621-646, 651-679); `vtw_apply_ctrl_options_live` (368) is a dead one-line wrapper.

*Risk:* Adding a fourth override field requires touching all four rollback sites; missing one leaves `g_ovr_mode/g_ovr_div` desynchronized from `g_ovr_active` after a failed CTRL readback.

#### S4. FTP modal duplicates the usb0 modal in config_menu.c
`ps_sources/frontend/config_menu.c:6938`

The FTP-sharing modal duplicates the usb0-SD-remote modal in three places: the input-swallowing block (6938-6947 vs 6949-6958), the full-screen warning panel (7932-7980 vs the usb0 panel, same geometry/layout), and the paired gating in `config_menu_draw` (8085-8097). One 'exclusive service modal' descriptor driving shared input/draw code would eliminate all three.

*Risk:* The two modals must stay behaviorally identical but nothing enforces it; a future exit-key or panel change silently misses one modal, and each additional exclusive mode multiplies the boilerplate.

#### S5. ONE//e start-acceptance sequence duplicated between manual start and persisted restore
`ps_sources/frontend/onee_service.c:277`

The start-acceptance sequence is duplicated between `onee_service_request_start` (217-237) and the persisted-restore path in `onee_service_poll` (277-289): same four-callback NULL check, same `can_start` test, same five-assignment commit. A single `onee_service_try_start()` helper would keep the 'exact same PL safety test as a manual start' property true by construction. `onee_service_mark_persisted` (84-96) is also a single-caller near-duplicate of `onee_service_force_persisted`.

*Risk:* A new precondition added to one copy and not the other lets a persisted-intent restore start the soft core under weaker checks than a manual start — the exact divergence this safety-critical module exists to prevent.

#### S6. INH-permitting mode set derived twice in apple_top.sv
`hdl/apple/apple_top.sv:876`

`machine_inh_allowed_wrapper_q` re-derives the mode-to-INH mapping independently of `machine_inh_allowed`: the assign at 880 computes `(machine_mode_q==1)||(machine_mode_q==2)` while the write decode at 2324 separately computes the same predicate on `wdata[1:0]` into the registered copy; `always_ff machine_inh_allowed_wrapper_q <= machine_inh_allowed;` (DONT_TOUCH retained) gives the same placement-friendly copy with one encoding.

*Risk:* The INH-permitting mode set lives in two hand-maintained expressions in a 2500-line file; a new machine mode or a mode-2 policy change lets the arbiter path (1990) and wrapper path (1036) silently disagree, giving different INH-permission views for one host — wrong INH pin behavior on one specific machine type.

### Efficiency

#### E1. FTP transfers are stop-and-wait
`ps_sources/frontend/ftp_sd_service.c:1031`

FTP transfers are stop-and-wait: RETR does one `f_read(1024)` then one SEND and waits for SENDOK (peer ACK) before the next chunk despite a 2KB TX window; LIST/NLST emits exactly one ~60-byte directory line per TCP segment per SENDOK round trip. Cheaper: size the chunk to the S1 TX window and keep writing while SN_TX_FSR reports free space; pack directory lines until the 1024-byte buffer is nearly full.

*Cost:* Downloads are capped near 1KB per client RTT (~1-2 Mbit/s on a quiet LAN, far worse on Wi-Fi) so a 32MB image takes minutes; a 300-file folder costs 300 client-ACK waits per refresh; every idle wait iteration still burns W5100 register reads (`read16_stable` is up to 9 MMIO block reads) through the Uthernet2 bridge.

#### E2. Seven ftp_sd_service_poll() call sites per modal iteration
`ps_sources/frontend/main.c:3538`

The FTP-modal branch calls `ftp_sd_service_poll()` from 7 separate sites per iteration (3538, 3573, 3596, 3617, 3630, 3652, 3655) to compensate for one-chunk-per-poll; each idle call performs control-socket status, send-ready, and data-accept register reads over MMIO. Cheaper: poll once per iteration and let the service iterate internally under a work budget while TX space/RX data remains.

*Cost:* With no client connected the loop issues dozens of indirect W5100 register reads per iteration doing nothing; with a client the scattered call sites are overhead around the real bottleneck.

#### E3. Overlay draw re-renders everything with uncached per-pixel access
`ps_sources/frontend/linear_text_overlay.c:226`

`linear_text_overlay_draw` re-renders the whole overlay every composited frame with two separate uncached loads per cell, per-pixel 16-bit stores to the non-cached output slot, and per-pixel clip/palette recomputation. Cheaper: one 16-bit read per cell, clip the cell rectangle once before the glyph loops, write two pixels per 32-bit store or burst a local row buffer.

*Cost:* An 80x24 overlay costs ~3840 uncached DDR reads plus ~215K uncached half-word writes per compose at up to 60 Hz on CPU0 — the DDR traffic class the compositor already throttles to 30 Hz elsewhere because full-frame uncached writes plus USB DMA pushed HP0 scanout into underruns; the overlay path bypasses that lesson and lengthens compositor_tick whenever visible.

#### E4. Per-byte mode-register read and patch scan in ROM shadow load
`ps_sources/frontend/vtw_service.c:339`

`vtw_shadow_load_fixed_rom` performs, per each of 16384 ROM bytes, an extra MMIO read of CARD_CTRL_ONEE_MODE_REG plus a 6-entry linear scan of `k_iiplus_rom_patches` on top of the shadow-data write. Cheaper: check isolation per 256-byte block and apply the six patches by direct offset after the copy.

*Cost:* The blocking ONE//e start/cold-reboot path roughly doubles its AXI round trips: ~16K added register reads add ~5-16ms of main-loop stall per ROM load (UART, USB HID, storage flush, compositor frozen); the same overhead runs on every physical-host session start (call at line 1041).

#### E5. Unconditional out-of-line overlay-capture call in CPU1's hot loop
`ps_sources/frontend/apple_cycle_egress.c:284`

The egress ring drain makes an unconditional out-of-line call to `linear_text_overlay_capture_on_write(a, d)` for every legacy bus-write record even though the function immediately returns when `s_capture_valid == 0` (the overwhelmingly common case). Cheaper: export the armed state as an extern flag or static-inline wrapper so the disarmed case is one predictable branch, matching the adjacent inlined generation counters.

*Cost:* This is CPU1's documented gap-prone hot loop: under vTW acceleration bus-write records arrive at multi-MHz rates, and ~5-10 cycles of call overhead per record eats headroom in exactly the path where holding CPU1 off the egress ring already caused capture gaps.

### Altitude / architecture

#### A1. Arbiter fast path uses raw positional magic numbers and a pinned SLICE
`hdl/apple/apple_bus_write_arbiter.sv:142`

The fast-path generalization uses raw positional magic numbers: `apple_top.sv:1985` passes FAST_DATA_CLIENT(2)/FAST_ADDR_CLIENT(11) as bare integers that must match positions in a 13-entry concatenation literal, with no named localparams and no elaboration-time assertion that index 11 is `vtw_ab_write`; the generic arbiter also hardcodes a device-specific placement `(* LOC = "SLICE_X112Y23", BEL = "A6LUT" *)` that belongs in the xdc.

*Risk:* The next client inserted left of position 2 silently shifts the fast INH-factoring onto the wrong client — compiles and simulates, but reintroduces the 133 MHz timing-closure failures the fast path was built to fix or mis-gates a serve; the pinned SLICE breaks on any part change, floorplan shuffle, or second instantiation.

#### A2. Slot-7 ownership policy scattered across five expressions
`hdl/apple/apple_top.sv:596`

Slot-7 ownership policy is scattered across five independent boolean expressions (`onee_slot7_cards_visible`, `onee_smartport_boot_owner`, the supersprite gate_ab expression, the `slot_rom_enable_mask` bit-7 term, `vtw_smartport_visible`) instead of one computed per-slot ownership vector consumed by all sites.

*Risk:* Any slot-7 change (new card, ONE//e SuperSprite support, cold-scan window change) requires consistently editing five expressions; missing one yields double bus drive on $C7xx or a card invisible only during the ONE//e cold slot scan, reproducing only on a cold boot of one mode/boot-target combination.

#### A3. ONE//e menu/pause/input policy is a distributed state machine
`ps_sources/frontend/main.c:2751`

ONE//e menu/pause/input policy is a distributed state machine glued together in the UI loop: main.c holds `g_onee_menu_paused` and must manually re-invoke `ui_sync_onee_menu_pause` from three places; usb_hid_service holds `g_onee_fixed_mode` + `g_onee_input_blocked`; vtw_service holds pause/running flags; config_menu holds `onee_mode_state`. `onee_service` exists but stops short of owning input routing and pause — the part every caller has to re-synchronize.

*Risk:* The next path that toggles menu visibility or input ownership and forgets the `ui_sync_onee_menu_pause` call leaves the 65C02 running while the menu owns the keyboard, or input permanently blocked after menu close — the unblock already depends on the fragile `all_input_released()` handshake ordering only main.c knows.

#### A4. Open-ended overlay register-space claims in the SmartPort card
`hdl/apple/smartport_card.sv:602`

Open-ended register-space claims: the overlay's PS write strobe is `awaddr >= 8'h20` with no upper bound and the AXI read mux's entire default arm returns `overlay_ps_rdata`, so the overlay implicitly owns all unallocated SmartPort register space; `apple_top.sv:214` likewise decodes the ONE//e input bridge as a raw literal range (0x5C-0x5F) instead of the CARD_CTRL_REG_ONEE_* localparams declared 200 lines below.

*Risk:* The next SmartPort register added at 0x29+ silently reads back overlay data and its writes strobe the overlay's `ps_wr_en`, corrupting armed overlay state only when the new feature is used mid-frame; a new card-control register allocated inside 0x5C-0x5F double-decodes into the input bridge.

### Conventions

#### C1. Ad hoc inline synchronizers instead of cdc_bit_sync.sv
`hdl/apple/onee_mode_safety_guard.sv:80`

New RTL builds ad hoc inline two-flop synchronizers (`(* ASYNC_REG *) apple_sync_meta/apple_sync_level`, `apple_power_sync`) instead of instancing the shared `cdc_bit_sync.sv` helper. AGENTS.md: 'Prefer shared CDC/reset helpers in hdl/ for new crossings (cdc_bit_sync.sv, cdc_bus_sampled.sv, reset_sync.sv) instead of ad hoc inline synchronizers.' Partial mitigation: cdc_bit_sync resets only to 0 while one bit needs APPLE_RESET_LEVELS=6'b100000; the other five bits and `apple_power_sync` could use the helper.

*Risk:* Ad hoc synchronizers evade the shared helper's ASYNC_REG/false-path conventions and the xdc patterns written against the helper's instance names.

#### C2. Asynchronous resets against the repo's own new sync-reset rule
`hdl/apple/onee_mode_safety_guard.sv:104`

This module uses asynchronous resets (`always_ff @(posedge clk or negedge resetn)` at 104, 123, 173, 194; `posedge physical_isolation_set` at 160) while every other new RTL module in the same commits uses synchronous resets, and its own header states the local clock runs continuously while powered. AGENTS.md (a rule this same diff adds): 'For new RTL, share clock enables and use synchronous resets when behavior permits.'

*Risk:* Violates the stated convention unless the safety-interlock role genuinely requires reset without a running clock — the header comment asserting the clock always runs argues it does not.

#### C3. Tab indentation in new axisimple_wrapper port lines
`hdl/axisimple/axisimple_wrapper.sv:185`

Five lines added to the axid_dev port list (`.S_AXI_WVALID` through `.S_AXI_WLAST`) are indented with tabs. AGENTS.md: 'Use 4-space indentation and keep alignment readable in HDL port lists and C macros.' Mitigating: the surrounding pre-existing file already uses tabs throughout.

*Risk:* Direct violation of the quoted rule, though consistent with the legacy file's existing style.

---

## Part 3 — Refuted candidates (for the record)

The verifier pass refuted these candidates with in-repo proof. Do not resurrect them without new evidence:

1. Skid-buffer fork
2. vTW posted-write/tick retiming
3. Aux-OE polarity
4. Bus-wrapper retiming
5. Virtual-bus ownership window
6. SmartPort prefetch removal
7. Overlay geometry overflow
8. FTP 226 drop
9. FTP stop-flush
10. Textoverlay status race
11. MMU narrowing
12. Vitis build-status gate

*Note:* The reviewer's tally reported 26 verified correctness candidates with verdicts 13 CONFIRMED / 3 PLAUSIBLE / 13 REFUTED; the counts include merged duplicates from the dedup step, and the refuted list above names 12 of the 13.

---

## Part 4 — Reviewer #2 independent assessment (2026-08-20)

### Review record

**Reviewer.** Reviewer #2, independent source and test review.

**Checkout.** `main` at `1f3994a`, covering the same three commits named at the top of this document.

**Method.** Reviewer #2 traced each Part 1 finding through the current source, checked every Part 2 cleanup candidate, reproduced the Windows quoting fault, checked the Apple IIe keyboard and reset rules against the Apple IIe Technical Reference Manual, and ran the focused tests listed below. Reviewer #2 did not change source code or the original review text.

**Summary.** Fifteen of the sixteen Part 1 items identify a real fault or design risk. Finding 12 is a documented policy choice, not a fault. Findings 10 and 15 are valid risks, but their stated failure or probability is not proved by the evidence in reviewer #1's text. Several Part 2 changes need a narrower implementation than reviewer #1 proposed.

### Part 1 verdicts from reviewer #2

| # | Reviewer #2 verdict | Fix? | Recommended action |
|---|---|---|---|
| 1 | **Confirmed.** `gate_ab` removes the phase strobes but preserves `res`. The SmartPort output register and overlay hold can therefore retain `wr_data_en` after slot-7 visibility drops. | **Yes — urgent.** | Pass an explicit visibility/ownership input into `smartport_card`. Clear `ab_write_q.wr_data_en` and `overlay_reply_hold_q` when ownership drops, and gate the public response while disabled. Test a visibility drop between `serve_en` and `data_en`. |
| 2 | **Confirmed.** `config_menu_poll_onee_mode()` runs before the FTP modal branch and can call FatFs while FTP owns the volume. | **Yes — urgent.** | Keep the pending ONE//e value in RAM during FTP. Do not run its retry timer or any FatFs call until FTP stops, then save and acknowledge it. Add a central SD-ownership guard for other background storage paths. |
| 3 | **Confirmed.** `onee_cold_slot_scan` treats the private warm reset like a cold restart and can leave slot 7 hidden because a normal warm start need not scan slot 6. | **Yes — high.** | Feed the warm-reset state into the cold-scan block. Rearm slot hiding only on ONE//e session start or explicit cold restart. Test warm reset after the first slot-6 scan and a separate cold-restart case. |
| 4 | **Confirmed.** The menu saves ON before the service accepts the start, while several refusal paths do not queue OFF. | **Yes — high.** | Add a non-mutating start preflight before saving ON. Recheck in the service to cover races. Every failed manual start after a save must queue and save OFF. Test every refusal reason and missing callback. |
| 5 | **Confirmed.** A HID trigger which rests at its logical minimum can remain in `raw_axis_active_mask` and block input release forever. | **Yes — high.** | Track the exact input action which closed the menu and wait for that source to release. Do not treat all absolute axes as centered controls. Add HID fixtures with minimum-rest triggers. |
| 6 | **Confirmed.** STOR truncates the target before the data peer is accepted, and a wrong peer destroys the pending data listener. | **Yes — urgent; data loss.** | Defer `FA_CREATE_ALWAYS` until the accepted data peer matches the control peer. Reject a wrong peer, reopen the listener, and preserve the pending transfer. Test a wrong peer followed by the right peer and verify that the old file stays intact until acceptance. |
| 7 | **Confirmed.** `overlay_drop_source_q` records a watched-rule level, not an actual overlay record request. A later unrelated full pulse can set STALE. | **Yes — high.** | Register the actual overlay request pulse and use it in the loss test. Cover full FIFO, pending-record collision, and a later full pulse with no new overlay event. |
| 8 | **Confirmed.** Both overlay buffers are occupied while SHOW waits for the frame edge, so accepting ARM can overwrite the pending buffer. | **Yes — high.** | Reject ARM as busy while `frame_pending_q` is set and define retry behavior. Test SHOW followed by ARM before the frame acknowledgement. |
| 9 | **Confirmed.** CLOSE_WAIT still permits server-to-client sends, but RETR and LIST fail before the send path runs. | **Yes.** | Continue the transmit state machine in ESTABLISHED and CLOSE_WAIT. Finish after EOF and all pending TX data; fail only on closed/error/timeout before completion. |
| 10 | **Confirmed as a design risk; observed corruption is not proved.** The async-cleared `onee_run_q` directly changes several internal muxes and card enables. A false lockout alone fails safely. | **Yes, before hardware signoff.** | Keep the raw asynchronous path only for immediate physical output isolation and a sticky kill latch. Synchronize the kill into the fabric and change internal mode at a clock or bus-cycle boundary. Constrain the intentional async pins and phase-sweep the raw inputs. Do not retain the broad async-Q fanout and merely add false paths. |
| 11 | **Confirmed.** ONE//e claims only C000 and C010, while C001-C00F should mirror the keyboard latch and C011-C01F should retain the keyboard code in bits 6:0. | **Yes — high compatibility impact.** | Return the keyboard latch for reads C000-C00F. Return the selected status bit plus `keyboard_code_q` for C011-C01F. Keep C010's current clear/any-key behavior. Apply the same low-bit rule to the vTW path or route these reads through the motherboard path. |
| 12 | **Rejected as a correctness finding.** Keeping Disk II selected while physical slot 6 is off is intentional for ONE//e. `config_menu_help.c` states that ONE//e keeps virtual Disk II and that a physical host falls back to SmartPort; `test_onee_config_menu.py` enforces both rules. | **No correctness fix.** | Do not restore coercion or rewrite the saved choice. An optional UI change may show `Disk II (host uses SmartPort)` in that physical-host state. |
| 13 | **Confirmed and reproduced.** A tool under `C:\Program Files` failed because the prequoted `call` command was quoted again by `subprocess.run`. | **Yes.** | Use one tested Windows command-line builder for the batch call. Do not pass a prequoted command as a list argument for another quoting pass. Test checkout, tool, and argument paths containing spaces. |
| 14 | **Confirmed.** The host harness reports a short asset but continues into fixed offsets and loops. | **Yes — low, test-only.** | Add `load_file_exact()` or return immediately after an exact-size check, before any index. Also reject negative `ftell` and allocation failure. |
| 15 | **Confirmed hazard; the stated 1-in-256 probability is not proved.** The probe reads DEVSEL E then F. On Disk II these are Q7 low and Q7 high, and F enters write mode. | **Yes; possible media damage.** | Put a read-only identity in the relocatable slot ROM. Scan Cnxx first and touch DEVSEL only after the ROM identity matches. A fixed slot-7 probe is an acceptable short-term limit. Test that an unknown Disk II slot receives no C0nE/C0nF probe. |
| 16 | **Confirmed.** Latest-build sorting uses only second-resolution `utc_start`, while build IDs have same-second suffixes. Promotion also rejects equal start times. | **Yes — low; normally fails closed.** | Use one ordering tuple such as `{utc_start, build_id}` for both pair selection and tested/confirm order, or record a higher-resolution time/sequence. Add three same-second fixture builds. |

Reviewer #2 used the Apple IIe keyboard and warm/cold reset descriptions in the [Apple IIe Technical Reference Manual](https://www.applelogic.org/files/AIIEREF.pdf) when checking findings 3 and 11.

### Part 2 cleanup assessment from reviewer #2

| Item | Reviewer #2 assessment | Action |
|---|---|---|
| R1 | **Correct.** FTP duplicates W5100 word access, command polling, and ring access. | Centralize these in a shared, socket-parameterized W5100 layer while fixing FTP. |
| R2 | **Correct duplication, with intentional input differences.** The VBL sources differ by design. | Fix finding 11 first, then share a pure status decoder which accepts all status bits, VBL, and keyboard low bits as inputs. |
| R3 | **Correct.** ARM-field unpacking is duplicated; slot selection differs intentionally. | Share only the ARM unpack helper. Keep slot choice in each caller. |
| R4 | **Correct.** `RGB565_CONST` duplicates `FB16_RGB`. | Include `fb16.h` and use `FB16_RGB`. |
| R5 | **Correct.** The repository now has 24 `vivado_tool` definitions. | Add a shared `scripts/xsim_runner.py` and migrate tests in small groups. |
| R6 | **Correct risk, but one implementation cannot serve both languages.** The Python and Tcl grammars already differ on whitespace and empty keys. | Write a strict manifest format and shared fixture set. Run cross-language parse and round-trip tests. Reject empty/duplicate keys and embedded newlines in both. |
| S1 | **Correct duplication.** | After FTP fixes, extract a bounded modal event pump with callbacks. Preserve the USB-only attention condition. |
| S2 | **Correct.** The send blocks and state reset repeat, and `transfer_eof` is dead in current control flow. | Share send/reset helpers. Either remove `transfer_eof` or make it a real pending-flush state as part of finding 9. |
| S3 | **Partly correct.** Rollback repeats, but `vtw_apply_ctrl_options_live` is not dead; two functions call it. | A small override snapshot structure is useful but low priority. |
| S4 | **Correct duplication.** | Share modal input and panel layout through a descriptor, while keeping service text and stop actions separate. |
| S5 | **Partly correct and safety-relevant.** Validation repeats, but manual start and saved-intent restore deliberately treat missing PL differently. | Share callback/status validation with a reason code, not the full state transition. Apply it with finding 4. |
| S6 | **Correct duplication; proposed fix is wrong.** `wrapper_q <= machine_inh_allowed` reads the old `machine_mode_q` and adds a one-cycle lag. | Use one pure `mode_allows_inh(mode)` function in both existing assignments. |
| E1 | **Correct performance issue; rates are estimates.** | Pack LIST lines and fill most of socket 1's 2 KB TX window per SEND. Measure before and after. |
| E2 | **Correct symptom and mainly a result of E1.** | Give the service one bounded internal work loop, then call it at clear scheduling points instead of seven scattered sites. |
| E3 | **Correct worst-case cost.** The 80x24 opaque case does about 215,000 pixel writes per compose. | Load one 16-bit cell, clip once per cell, and render row spans. Use wider stores only after checking alignment and memory attributes. Measure compositor time and scanout health. |
| E4 | **Partly correct; proposed safety relaxation is not justified.** The patch scan is wasteful, but the per-byte isolation check is deliberate. | Apply six patches directly. Keep the per-byte guard unless a tested response-time bound permits a coarser check. |
| E5 | **Plausible but unmeasured.** | Profile CPU1 first. If material, add a safe inline fast flag or link-time inlining; do not expose a flag which the compiler may cache incorrectly. |
| A1 | **Correct.** Client indices and the device-specific LOC are fragile. | Name each client index, add elaboration checks, and move placement to XDC with an exact cell query and missing-object failure. |
| A2 | **Mostly correct.** Boot ownership, card visibility, and vTW routing cannot all become one Boolean. | Derive named policy signals from one state block and add mutual-exclusion assertions. |
| A3 | **Correct.** Pause and input ownership form a state machine spread across modules. | After finding 5, let one controller own pause, input block, and release-wait state; let `main.c` observe it. |
| A4 | **Correct and worth fixing now.** | Bound overlay reads and writes to 0x20-0x28. Return the normal unmapped value outside that range. Move the ONE//e constants before the bridge decode and replace 0x5C-0x5F literals. |
| C1 | **Partly correct.** It breaks the repository preference, but reviewer #1 overstates the XDC benefit; there is no general constraint tied to every `cdc_bit_sync` instance. | Extend the helper with a reset-value parameter and use it where the finding-10 design allows. |
| C2 | **Rejected as a blanket change.** Immediate physical isolation and kill latching need asynchronous assertion. | Use synchronous reset for ordinary state. Keep intentional async assertion with synchronized release, clear comments, and explicit timing constraints. |
| C3 | **Correct fact, no useful stand-alone fix.** The surrounding legacy file uses tabs. | Convert the whole file under a formatter later rather than mixing indentation styles now. |

### Part 3 record quality assessment from reviewer #2

Reviewer #1 lists only the names of the refuted candidates. The document does not include the original claims, files, lines, failure cases, or verifier proof, and no other review record in the repository supplies them. Reviewer #2 therefore cannot independently confirm any exact refutation from this record. None should cause a code change without the missing claim and evidence.

| Refuted candidate | Reviewer #2 disposition |
|---|---|
| Skid-buffer fork | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| vTW posted-write/tick retiming | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Aux-OE polarity | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Bus-wrapper retiming | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Virtual-bus ownership window | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| SmartPort prefetch removal | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Overlay geometry overflow | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| FTP 226 drop | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| FTP stop-flush | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Textoverlay status race | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| MMU narrowing | No fix on the evidence recorded here. Reopen only with the original claim and proof. |
| Vitis build-status gate | No fix on the evidence recorded here. Reopen only with the original claim and proof. |

Reviewer #2 also notes that the Part 3 tally is internally inconsistent: 13 confirmed + 3 plausible + 13 refuted equals 29, not 26, and the appendix names only 12 refuted items. This note does not alter reviewer #1's preserved record.

### Reviewer #2 fix order

1. Tools and tests: findings 13, 14, and 16.
2. Bus or data loss: findings 1, 2, 6, and 15.
3. ONE//e and overlay correctness: findings 3, 4, 5, 7, 8, and 11.
4. Network and safety behavior: findings 9 and 10.
5. Cleanup tied to those fixes: R1, R2, R6, S2, S5, A3, and A4.

### Reviewer #2 validation record

The following tests passed on `1f3994a`:

- `python scripts/test_ftp_sd_service.py`
- `python scripts/test_onee_config_menu.py`
- `python scripts/test_timing_firmware_packaging.py`
- `python scripts/test_smartport_config_menu.py`
- `python scripts/test_linear_text_overlay.py`
- `python scripts/test_onee_mode_safety_guard.py`
- `python scripts/test_onee_motherboard_io.py`
- `python scripts/test_onee_input_service.py`
- `python scripts/test_onee_disk2_boot.py`, including DOS 3.3 System Master and ProDOS 2.4.3 boot simulations

These existing tests do not exercise the reported edge cases, so their success does not clear the findings. The plain Tcl timing tests did not run because `tclsh` was not present on `PATH`. Reviewer #2 performed no hardware test and made no source-code change.

### Reviewer #2 implementation record

**Branch.** `codex/review-fixes-20260820`, based on `main` at `1f3994a`.

**Scope.** Work followed the revised order requested after the review: tools and tests first, then the four other fix groups. Finding 12 stayed unchanged because reviewer #2 rejected it as a correctness fault. Cleanup outside R1, R2, R6, S2, S5, A3, and A4 stayed out of scope.

#### Logical fix groups and commits

1. **Tools and tests first: findings 13, 14, and 16.** Commit `2314a28` (`Fix review tooling edge cases`) fixes Windows batch quoting, rejects short render assets before indexed access, and gives same-second timing builds one stable order. Tests cover spaced paths, short files, and tied build times.
2. **Bus and data isolation: findings 1, 2, 6, and 15.** Commit `16aa381` (`Fix bus and data isolation`) clears and gates stale SmartPort replies when visibility drops, defers ONE//e config storage work while FTP owns the SD volume, preserves a STOR target and listener until the correct data peer connects, and makes the overlay utility identify the card through slot ROM before it touches DEVSEL registers.
3. **ONE//e and overlay state: findings 3, 4, 5, 7, 8, and 11.** Commit `a01f50e` (`Fix ONE//e and overlay state handling`) separates warm reset from cold slot scanning, validates a ONE//e start before saving ON and rolls saved state back on refusal, ties menu release to the closing input instead of all absolute axes, records only a suppressed overlay push as a drop, blocks ARM while SHOW is pending, and implements the Apple IIe keyboard mirrors and status low bits.
4. **Network and safety state: findings 9 and 10.** Commit `2f06ff9` (`Fix network and safety state handling`) lets RETR/LIST drain TX data in CLOSE_WAIT and splits immediate physical isolation from synchronized internal ONE//e shutdown. Commit `e761495` (`Bind ONEe safety timing exception`) binds the intentional raw transition capture to the six synthesized FDPE preset pins. A focused synthesized-netlist query found exactly six cells and six PRE pins.
5. **Cleanup tied to the fixes.** The following commits keep the fixes shared and bounded:
   - `fc9d7a9` (`Share W5100 socket helpers`): R1 and S2.
   - `46b5aa7` (`Share Apple IIe status decoding`): R2.
   - `03b57ec` (`Define strict timing manifest format`): R6.
   - `4c121aa` (`Centralize ONEe UI policy`): S5 and A3.
   - `bed8c45` (`Bound card-control register ranges`): A4.
   - `bb4da2e` (`Align USB tests with ONEe policy`) and `a3368dc` (`Refresh ONEe source assertions`): update source checks for the new ownership rules.
   - `d0601e1` (`Fix timing utilization parser`) and `83c32ad` (`Store one-line Vivado version`): fix strict-manifest faults found by the real Vivado flow.

Commit `18b8d34` (`Record second code review`) preserves reviewer #2's assessment before implementation. Each group above was staged and committed on its own; no fix commit mixes in the pre-existing untracked `.claude/` directory.

#### Validation after implementation

The focused and broad source, native, and RTL suites passed. This included:

- FTP, ONE//e config and UI policy, timing manifest and firmware packaging, SmartPort service and ROM, USB HID, ONE//e safety, motherboard I/O, bus integration, video, input, speaker, runtime, and top-I/O tests.
- Real-ROM ONE//e video boot-target tests and both DOS 3.3 System Master and ProDOS 2.4.3 Disk II boot simulations.
- Uthernet II, VidHD SHR, linear text overlay, Disk II standard, the host render harness, and all 20 vTW benches.
- `vivado -mode batch -source scripts/test_timing_tooling.tcl`.
- `vitis -s .\scripts\create_vitis_workspace.py`, which rebuilt the frontend, core1, and bootloader successfully. It emitted only the existing compiler and path warnings.

`scripts/test_capture_fifos.py` passed its registered checks but still printed its existing diagnostic that a nonzero jam address freezes after 65 events while an RTL comment says 64. That issue is outside this review and was not changed.

#### Full Vivado result

The final clean-placement validation used `APPLETINI_FULL_BUILD=1` with `scripts/build_and_export_xsa.tcl`. Build record `20260820T113722Z-e761495f-full` reports:

- synthesis: 0 errors and 0 critical warnings;
- `missing_constraint_objects=0`, `unconstrained_internal_endpoints=0`;
- route and bus skew: PASS, with 0 route errors and bus-skew WNS +5.286 ns;
- hold WNS +0.035 ns and pulse-width WNS +0.265 ns;
- setup WNS -0.072 ns, TNS -0.072 ns, one failing setup endpoint after the one allowed AggressiveExplore rescue pass;
- final status `failed`; no candidate checkpoint, bitstream, or XSA was exported.

This full build validates source load, synthesis, the finding-10 constraint binding, routing, reports, and the strict fail-closed export path. It is not a timing-clean or promotable build. The pre-existing untracked `.claude/` directory also made the recorded `git_dirty=1`; reviewer #2 did not modify or stage it. No card was flashed, and no hardware test was run.

#### Timing regression follow-up

**Reason for the follow-up.** The first implementation run above was worse than the pre-branch design. The user also reported that Arekkusu's video soft-switch test disk showed varying delays on branch firmware while the prior build showed zero for every test. Reviewer #2 therefore stopped treating route repair as proof of a good fix and made a same-tool, full-build comparison against `main`.

**Rollback and fault isolation.** Commit `8671616` (`Revert unstable timing pipeline changes`) removed all eighteen speculative timing and pipeline commits made after the review fixes. Its tree matched `c5a0eea` exactly. This removed added bus and video stages before any further timing work. Later builds isolated a direct SmartPort visibility mask in the shared Apple response path: the mask let slot-7 ownership change between the address and data parts of one Apple cycle and placed SmartPort state in the vTW-to-mouse timing cone. Commits `7853948`, `49d5b20`, and `a301776` narrowed that state and its reset rule. Commit `a78f51e` (`Hold slot 7 ownership for each bus cycle`) replaced the live mask with an owner chosen at `addr_en` and held through the cycle. It adds no Apple bus or video pipeline stage.

**Measured A/B record.** Each listed run used Vivado 2025.2, a fresh project, `APPLETINI_FULL_BUILD=1`, no incremental checkpoint, and the same Explore implementation flow.

| Source | Build record | Setup WNS | Hold WNS | Result |
|---|---|---:|---:|---|
| Pre-branch `main` at `1f3994a` | `20260820T152835Z-1f3994ab-full` | +0.105 ns | +0.024 ns | Clean route and exported XSA; measured baseline. |
| Initial review implementation at `e761495` | `20260820T113722Z-e761495f-full` | -0.072 ns | +0.035 ns | Failed; no XSA. |
| Cycle-stable slot-7 owner at `a78f51e` | `20260820T181656Z-a78f51e9-full` | +0.076 ns | +0.044 ns | Clean route and exported XSA. |
| Bounded ONE//e run-state fanout at `ef35588` | `20260820T183523Z-ef355889-full` | +0.114 ns | +0.045 ns | Clean route and exported XSA; accepted timing state. |
| Audio-only pipeline trial at `9a8eced` | `20260820T185113Z-9a8eced8-full` | +0.012 ns | +0.059 ns | Clean but too close to failure; rejected and reverted by `74d70ae`. |

The measured pre-branch value is +0.105 ns, close to the user's recalled value of about +0.125 ns. The accepted +0.114 ns result is +0.009 ns above that measured baseline. All build manifests record `git_dirty=1` only because of the pre-existing, untracked `.claude/` directory; no review commit stages it.

**Worst-path record.** The pre-branch top paths ran from Disk II drive-select state into the vTW state machine at +0.105 ns. The first review build moved the worst path to machine-mode and physical-bus direction logic at -0.072 ns. Before `a78f51e`, the routed top paths ran from vTW cycle-address state through the shared response path into mouse registers; the direct SmartPort visibility mask was in that cone. After the cycle-owner fix, the worst path was a 79%-routing path from the high-fanout ONE//e run state through the virtual write-data chain into a mouse register at +0.076 ns. Commit `ef35588` limits only that state bit's fanout, adds no clocked stage, and raised WNS to +0.114 ns. The next ten paths were then an audio-only pair of saturating adders. Splitting those adders removed the target cone but lowered overall WNS to +0.012 ns, so reviewer #2 reverted the trial.

The code tree after revert `74d70ae` is source-identical to tested commit `ef35588`; `git diff --exit-code ef35588..74d70ae` is empty. The later review-document commit changes no code. The +0.114 ns build therefore proves the current source tree, but it is not an exact-current-commit build record. A release build must still run from the final release commit.

**Regression tests after the timing correction.** The following passed on the accepted source tree:

- `python scripts/test_onee_mode_safety_guard.py`
- `python scripts/test_onee_bus_integration.py`
- `python scripts/test_supersprite_card.py` (9 tests)
- `python scripts/test_vidhd_shr.py` (17 tests)
- `python scripts/test_linear_text_overlay.py`, including its RTL bench
- `python scripts/test_vtw.py` (all 20 RTL benches)

The rejected audio trial also passed `test_onee_top_io.py`, `test_onee_speaker_audio.py`, the 9 SuperSprite tests, and the 20 Disk II standard tests before timing rejected it. Those two changed files were restored by `74d70ae`.

**Video soft-switch hardware gate.** Source and simulation tests cannot clear the reported variable video delays. Reviewer #2 will not add per-mode delay offsets. The acceptance test is the unmodified Arekkusu disk on hardware: every reported video soft-switch delay must return to zero against the known pre-branch firmware. No card was flashed during this review, so that check remains open. The current source contains no added Apple bus or video pipeline from the rejected timing work.

**Disposition.** At the user's direction, reviewer #2 stopped further timing changes after restoring the +0.114 ns source. This is near the pre-branch timing level and is suitable for continuing the bug fixes, but it is not timing signoff: it remains below the +0.300 ns promotion target, lacks two same-commit qualifying full builds, and lacks the zero-delay hardware result.
