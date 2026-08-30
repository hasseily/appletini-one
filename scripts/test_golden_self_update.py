#!/usr/bin/env python3
"""Source checks for the power-fail-safe golden self-update path."""

from __future__ import annotations

import tempfile
from pathlib import Path

try:
    from . import image_manifest
    from . import serial_firmware_update
except ImportError:  # Direct script execution.
    import image_manifest
    import serial_firmware_update


REPO_ROOT = Path(__file__).resolve().parents[1]
SERIAL_BOOT_C = REPO_ROOT / "ps_sources" / "bootloader" / "serial_boot.c"
SERIAL_BOOT_H = REPO_ROOT / "ps_sources" / "bootloader" / "serial_boot.h"
BOOT_MAIN_C = REPO_ROOT / "ps_sources" / "bootloader" / "main.c"
SELF_UPDATE_C = REPO_ROOT / "ps_sources" / "lib" / "golden_self_update.c"
SELF_UPDATE_H = REPO_ROOT / "ps_sources" / "lib" / "golden_self_update.h"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
UART_CONTROL_C = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c"
UART_CONTROL_H = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.h"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    start_at = source.find(start)
    end_at = source.find(end, start_at + len(start))
    require(start_at >= 0 and end_at > start_at,
            f"missing source section from {start!r} to {end!r}")
    return source[start_at:end_at]


def test_monitor_command_contract() -> None:
    source = read(SERIAL_BOOT_C)

    require("selfupdate    - stage SD BOOT.BIN" in source and
            "selfrx <size> <crc32>" in source,
            "monitor help must list both golden update commands")
    require("selfrx requires a nonzero size and an explicit CRC32" in source,
            "monitor help must state the exact selfrx argument rule")

    sd_command = section(
        source,
        'if (str_ieq(argv[0], "selfupdate"))',
        'if (str_ieq(argv[0], "selfrx"))',
    )
    require("argc != 1" in sd_command and
            "run_golden_self_update_from_sd(uart_base)" in sd_command,
            "selfupdate must reject extra arguments and use the SD updater")

    rx_command = section(
        source,
        'if (str_ieq(argv[0], "selfrx"))',
        'if (str_ieq(argv[0], "rx") || str_ieq(argv[0], "receive"))',
    )
    require("argc != 3" in rx_command and
            "parse_u32(argv[1], &expected_size)" in rx_command and
            "parse_u32(argv[2], &expected_crc)" in rx_command,
            "selfrx must require and parse exactly size plus CRC32")
    require("expected_size == 0U" in rx_command,
            "selfrx must reject an empty image")
    require("expected_crc == 0U" not in rx_command,
            "CRC32 value zero is valid when the argument is present")
    require("expected_size > GOLDEN_SELF_UPDATE_SLOT_SIZE" in rx_command,
            "selfrx must reject images larger than one golden A/B slot")
    require(rx_command.index("receive_golden_xmodem") <
            rx_command.index("run_golden_self_update_from_memory"),
            "selfrx must finish and verify receive before it starts flash update")


def test_xmodem_targets_keep_firmware_rx_on_sd() -> None:
    source = read(SERIAL_BOOT_C)

    require("static uint8_t g_golden_rx_buf[GOLDEN_SELF_UPDATE_SLOT_SIZE]" in source and
            "__attribute__((aligned(64)))" in source,
            "selfrx must reserve an aligned full-slot DDR .bss buffer")
    require("SERIAL_RX_TARGET_SD_FILE" in source and
            "SERIAL_RX_TARGET_MEMORY" in source,
            "the shared XMODEM receiver must support SD and memory targets")

    receiver = section(
        source,
        "static int receive_xmodem(",
        "static int receive_firmware_xmodem(",
    )
    require("if (target->kind == SERIAL_RX_TARGET_MEMORY)" in receiver and
            "memcpy(&target->memory[total_written], g_xmodem_buf, to_write);" in receiver,
            "memory receive must copy validated XMODEM payload bytes into DDR")
    require("f_write(&f, g_xmodem_buf" in receiver and
            "f_rename(target->tmp_path, target->final_path)" in receiver,
            "SD receive must keep its temp-file and rename flow")
    require("total_written != expected_size" in receiver and
            "crc32 != expected_crc" in receiver,
            "the shared receiver must enforce exact size and whole-file CRC when supplied")
    eot = section(receiver, "if (header == XMODEM_EOT)",
                  "if (header == XMODEM_CAN)")
    require(eot.index("total_written != expected_size") <
            eot.index("crc32 != expected_crc") <
            eot.index("f_sync(&f)") <
            eot.index("f_rename(target->tmp_path, target->final_path)") <
            eot.index("uart_putc_one(uart_base, (char)XMODEM_ACK)"),
            "EOT ACK must wait for size, CRC, sync, and final rename")
    require("goto out_abort;" in eot and "goto out_unlink;" not in eot,
            "every EOT validation failure must reach the CAN abort path")

    firmware = section(
        source,
        "static int receive_firmware_xmodem(",
        "static int receive_golden_xmodem(",
    )
    require("target.kind = SERIAL_RX_TARGET_SD_FILE;" in firmware and
            "target.max_size = layout->firmware.size;" in firmware and
            "target.tmp_path = SERIAL_BOOT_RX_TMP_PATH;" in firmware and
            "target.final_path = SERIAL_BOOT_FW_PATH;" in firmware,
            "legacy rx must still stage FIRMWARE.BIN on SD within the firmware slot limit")

    golden = section(
        source,
        "static int receive_golden_xmodem(",
        "static serial_boot_action_t run_golden_self_update_from_sd(",
    )
    require("target.kind = SERIAL_RX_TARGET_MEMORY;" in golden and
            "target.max_size = GOLDEN_SELF_UPDATE_SLOT_SIZE;" in golden and
            "target.memory = g_golden_rx_buf;" in golden and
            "target.tmp_path = NULL;" in golden and
            "target.final_path = NULL;" in golden,
            "selfrx must not depend on an SD file")


def test_update_entry_points_and_trial_reboot() -> None:
    serial = read(SERIAL_BOOT_C)
    header = read(SERIAL_BOOT_H)
    main = read(BOOT_MAIN_C)

    require('#define SERIAL_BOOT_GOLDEN_PATH       "0:/BOOT.BIN"' in serial and
            "SERIAL_BOOT_GOLDEN_DONE_PATH" not in serial,
            "selfupdate must retain BOOT.BIN until trial repair finishes")
    require("golden_self_update_run(SERIAL_BOOT_GOLDEN_PATH," in serial and
            "golden_self_update_run_memory(g_golden_rx_buf," in serial,
            "SD and DDR monitor commands must call their matching updater entry points")
    require("g_staged_boot_offset = result.boot_offset;" in serial and
            "SERIAL_BOOT_ACTION_BOOT_GOLDEN_TRIAL" in header,
            "serial monitor must return a distinct verified-update action")

    reboot = section(
        main,
        "if (action == SERIAL_BOOT_ACTION_BOOT_GOLDEN_TRIAL)",
        "if (action == SERIAL_BOOT_ACTION_BOOT_FIRMWARE)",
    )
    require("serial_boot_staged_boot_offset()" in reboot and
            "golden_led_off();" in reboot and
            "boot_control_boot_qspi_image_offset(boot_offset);" in reboot,
            "successful stage must boot the exact verified B offset")
    require("boot_control_soft_reset" not in reboot,
            "successful golden update must not use a plain soft reset")


def test_trial_boot_repairs_primary_before_monitor() -> None:
    main = read(BOOT_MAIN_C)

    cache = "current_boot_offset = boot_control_current_qspi_image_offset();"
    clear = "boot_control_select_qspi_image_offset(GOLDEN_SELF_UPDATE_PRIMARY_OFFSET);"
    trial = "if (current_boot_offset == GOLDEN_SELF_UPDATE_TRIAL_OFFSET)"
    repair = "golden_self_update_repair_primary(current_boot_offset,"
    first_monitor = "serial_boot_menu(layout, GOLDEN_SERIAL_BOOT_WINDOW_SECONDS)"
    require(main.index(cache) < main.index(clear) < main.index(trial) <
            main.index(repair) < main.index(first_monitor),
            "boot must cache B, select A for future resets, then repair before the serial window")

    trial_flow = section(main, trial, "for (;;) {")
    require("repair_result.updated != 0" in trial_flow and
            "repair_result.verified != 0" in trial_flow and
            "repair_result.reset_required != 0" in trial_flow and
            "repair_result.boot_offset == GOLDEN_SELF_UPDATE_PRIMARY_OFFSET" in trial_flow and
            "boot_control_boot_qspi_image_offset(repair_result.boot_offset);" in trial_flow,
            "a verified B repair must reboot to the result's A offset")
    require("action = serial_boot_menu(layout, 0U);" in trial_flow,
            "B repair failure must remain in the indefinite monitor")


def test_shared_reader_frontends_use_one_update_engine() -> None:
    source = read(SELF_UPDATE_C)
    header = read(SELF_UPDATE_H)

    require("typedef int (*image_read_fn)" in source and
            "image_read_fn read;" in source,
            "updater must abstract source reads through a checked callback")
    file_frontend = section(
        source,
        "int golden_self_update_run(const char *source_path,",
        "int golden_self_update_run_memory(",
    )
    memory_frontend = source[source.index("int golden_self_update_run_memory("):]
    require("reader.read = file_read;" in file_frontend and
            "golden_self_update_common(&reader, out)" in file_frontend,
            "file front end must feed the common updater through file_read")
    require("f_rename(" not in file_frontend and "f_unlink(" not in file_frontend and
            "Source file retained until primary repair completes" in file_frontend,
            "staging must retain BOOT.BIN so a cut before trial repair can retry")
    require("reader.read = memory_read;" in memory_frontend and
            "golden_self_update_common(&reader, out)" in memory_frontend,
            "memory front end must feed the same common updater through memory_read")
    require("raw Zynq Bootgen image" in header and
            "32-byte golden_self_update_manifest_t trailer" in header,
            "public API must state the source image and manifest contract")


def test_running_firmware_selfupdate_callback_boots_verified_trial() -> None:
    header = read(UART_CONTROL_H)
    control = read(UART_CONTROL_C)
    frontend = read(FRONTEND_MAIN_C)

    require("int (*self_update_boot)(void *ctx," in header and
            "uint32_t *boot_offset_out);" in header and
            "void (*reboot_qspi_image)(void *ctx, uint32_t boot_offset);" in header,
            "UART ops must carry the verified updater boot offset to a reboot callback")

    command = section(
        control,
        'if (str_ieq(argv[0], "selfupdate"))',
        'if (str_ieq(argv[0], "reboot") || str_ieq(argv[0], "reset"))',
    )
    require("argc != 1" in command and
            "config_menu_usb0_sd_remote_active" in command and
            "config_menu_ethernet_ftp_sd_remote_active" in command,
            "running firmware selfupdate must reject arguments and active SD sharing")
    require(command.index("ops->self_update_boot(ops->ctx,") <
            command.index("ops->reboot_qspi_image(ops->ctx, boot_offset);"),
            "UART command must reboot only after its self-update callback succeeds")

    callback = section(frontend, "static int control_self_update_boot(",
                       "static const uart_control_ops_t g_uart_control_ops")
    require('golden_self_update_run("0:/BOOT.BIN",' in callback and
            '"0:/BOOT.OK"' not in callback and
            "result.updated == 0" in callback and
            "result.verified == 0" in callback and
            "result.reset_required == 0" in callback and
            "result.boot_offset != GOLDEN_SELF_UPDATE_TRIAL_OFFSET" in callback and
            "*boot_offset_out = result.boot_offset;" in callback,
            "frontend callback must return only the shared updater's verified B offset")
    require(".self_update_boot = control_self_update_boot," in frontend and
            ".reboot_qspi_image = control_reboot_qspi_image," in frontend,
            "frontend must bind both update and reboot-to-offset callbacks")


def test_fixed_slots_and_fault_cut_model() -> None:
    header = read(SELF_UPDATE_H)

    require("GOLDEN_SELF_UPDATE_PRIMARY_OFFSET           0x00000000U" in header and
            "GOLDEN_SELF_UPDATE_TRIAL_OFFSET             0x00100000U" in header and
            "GOLDEN_SELF_UPDATE_SLOT_SIZE                0x00100000U" in header,
            "golden A/B layout must stay fixed at 0 and 1 MiB")

    primary = 0x00000000
    trial = 0x00100000

    def por_boot(a_valid: bool, b_valid: bool,
                 explicit_offset: int | None = None) -> int | None:
        if explicit_offset == trial and b_valid:
            return trial
        if a_valid:
            return primary
        if b_valid:
            return trial
        return None

    cuts = [
        ("before B commit", True, False, None, primary),
        ("after B commit before trial reset", True, True, None, primary),
        ("explicit multiboot B", True, True, trial, trial),
        ("A ID invalid", False, True, None, trial),
        ("A erased", False, True, None, trial),
        ("A programmed with ID withheld", False, True, None, trial),
        ("A ID-last commit", True, True, None, primary),
    ]
    for label, a_valid, b_valid, explicit, expected in cuts:
        require(por_boot(a_valid, b_valid, explicit) == expected,
                f"fault-cut model chose the wrong image {label}")


def test_preflight_and_ab_commit_order() -> None:
    source = read(SELF_UPDATE_C)
    stage = section(
        source,
        "static int golden_self_update_common(",
        "int golden_self_update_repair_primary(",
    )
    stage_order = [
        "target_preflight(reader, &target, out)",
        "primary_bootgen_preflight(&nor, out)",
        "fallback_preflight(&nor, capacity, out)",
        "install_slot_image(&nor, TRIAL_FLASH_OFFSET, reader, &target,",
        "out->boot_offset = TRIAL_FLASH_OFFSET;",
    ]
    stage_positions = [stage.find(item) for item in stage_order]
    require(all(pos >= 0 for pos in stage_positions) and
            stage_positions == sorted(stage_positions),
            "stage must validate A and fallback before it installs and boots B")
    require("install_slot_image(&nor, PRIMARY_FLASH_OFFSET" not in stage,
            "stage must never erase or program A")

    repair = section(
        source,
        "int golden_self_update_repair_primary(",
        "int golden_self_update_run(const char *source_path,",
    )
    repair_order = [
        "flash_golden_preflight(&nor, TRIAL_FLASH_OFFSET, &trial,",
        "fallback_preflight(&nor, capacity, out)",
        "install_slot_image(&nor, PRIMARY_FLASH_OFFSET,",
    ]
    repair_positions = [repair.find(item) for item in repair_order]
    require(all(pos >= 0 for pos in repair_positions) and
            repair_positions == sorted(repair_positions),
            "repair must validate running B and fallback before it replaces A")
    install_primary = repair.find("install_slot_image(&nor, PRIMARY_FLASH_OFFSET,")
    require(repair.find("out->boot_offset = PRIMARY_FLASH_OFFSET;", install_primary) >
            install_primary,
            "successful repair must request a reset to A")
    require("qspi_nor_erase_region(&nor, TRIAL_FLASH_OFFSET" not in repair and
            "invalidate_slot_ident(&nor, TRIAL_FLASH_OFFSET" not in repair and
            "install_slot_image(&nor, TRIAL_FLASH_OFFSET" not in repair,
            "repair must leave verified B valid after A commits")

    installer = section(source, "static int install_slot_image(",
                        "static int golden_self_update_common(")
    commit_order = [
        "qspi_nor_erase_region(nor, slot_offset, GOLDEN_SLOT_SIZE)",
        "program_target_except_ident(nor, slot_offset, reader,",
        "verify_target(nor, slot_offset, reader, target, 1,",
        "intermediate_slot_headers_invalid(nor, slot_offset)",
        "qspi_nor_program(nor, slot_offset + BOOT_IDENT_WORD_OFFSET,",
        "final_slot_header_valid(nor, slot_offset)",
        "verify_target(nor, slot_offset, reader, target, 0,",
    ]
    commit_positions = [installer.find(item) for item in commit_order]
    require(all(pos >= 0 for pos in commit_positions) and
            commit_positions == sorted(commit_positions),
            "each slot install must commit XLNX last after intermediate verification")

    target = section(source, "static int target_preflight(",
                     "static int fallback_preflight(")
    require("target->manifest.role != GOLDEN_SELF_UPDATE_ROLE_GOLDEN" in target and
            "GOLDEN_SELF_UPDATE_FLAG_RECOVERY" in target and
            "target->manifest.payload_size" in target and
            "bootgen_image_validate" in target and
            "alternate_boot_header_present" in target and
            "target payload CRC32 mismatch" in target and
            "target recovery marker is missing" in target,
            "target preflight must prove role, recovery support, shape, CRC, and marker")

    fallback = section(source, "static int fallback_preflight(",
                       "static int program_target_except_ident(")
    require("manifest.role != GOLDEN_SELF_UPDATE_ROLE_FIRMWARE" in fallback and
            "GOLDEN_SELF_UPDATE_FLAG_RECOVERY" in fallback and
            "fallback payload CRC32 mismatch" in fallback and
            "fallback recovery marker is missing" in fallback,
            "fallback preflight must prove a valid recovery firmware before any golden write")

    writer = section(source, "static int program_target_except_ident(",
                     "static int verify_target(")
    require("BOOT_IDENT_WORD_OFFSET + 4U" in writer and
            "g_source_buf + (after_offset - offset)" in writer,
            "bulk programming must skip all four golden ID bytes")


def test_host_golden_mode_validates_manifest_and_uses_selfrx() -> None:
    host_source = read(REPO_ROOT / "scripts" / "serial_firmware_update.py")

    require('ap.add_argument("--golden", action="store_true"' in host_source and
            'default_name = "BOOT.BIN" if golden else "FIRMWARE.BIN"' in host_source,
            "host tool must select BOOT.BIN only in explicit golden mode")
    require("expected_role=image_manifest.ROLE_GOLDEN" in host_source and
            "expected_recovery=True" in host_source,
            "host tool must verify golden role and recovery flag before opening serial")
    require("image_size > GOLDEN_IMAGE_MAX_BYTES" in host_source,
            "host tool must reject a verified image which cannot fit in B")
    require('command_name = "selfrx" if args.golden else "rx"' in host_source,
            "host tool must preserve rx by default and use selfrx for golden images")

    with tempfile.TemporaryDirectory() as temp_name:
        temp = Path(temp_name)
        payload = b"golden host validation payload"
        raw = temp / "raw.bin"
        golden = temp / "BOOT.BIN"
        wrong_role = temp / "FIRMWARE.BIN"
        no_recovery = temp / "BOOT-NORECOVERY.BIN"
        raw.write_bytes(payload)
        image_manifest.append_manifest(
            raw,
            golden,
            role=image_manifest.ROLE_GOLDEN,
            flags=image_manifest.FLAG_RECOVERY_CAPABLE,
        )
        image_manifest.append_manifest(
            raw,
            wrong_role,
            role=image_manifest.ROLE_FIRMWARE,
            flags=image_manifest.FLAG_RECOVERY_CAPABLE,
        )
        image_manifest.append_manifest(
            raw,
            no_recovery,
            role=image_manifest.ROLE_GOLDEN,
        )

        path, loaded = serial_firmware_update.load_upload_image(str(golden), True)
        require(path == golden and loaded == golden.read_bytes(),
                "host golden mode must return the locally verified full image")
        for invalid in (wrong_role, no_recovery):
            try:
                serial_firmware_update.load_upload_image(str(invalid), True)
            except image_manifest.ManifestError:
                pass
            else:
                raise TestFailure(f"host golden mode accepted {invalid.name}")

        plain = temp / "plain-firmware.bin"
        plain.write_bytes(b"legacy firmware payload")
        path, loaded = serial_firmware_update.load_upload_image(str(plain), False)
        require(path == plain and loaded == plain.read_bytes(),
                "default firmware mode must not impose the golden manifest contract")

        large_raw = temp / "large-raw.bin"
        oversized = temp / "oversized-golden.bin"
        large_raw.write_bytes(
            b"X" * (serial_firmware_update.GOLDEN_IMAGE_MAX_BYTES + 1 -
                    image_manifest.MANIFEST_SIZE)
        )
        image_manifest.append_manifest(
            large_raw,
            oversized,
            role=image_manifest.ROLE_GOLDEN,
            flags=image_manifest.FLAG_RECOVERY_CAPABLE,
        )
        try:
            serial_firmware_update.load_upload_image(str(oversized), True)
        except image_manifest.ManifestError:
            pass
        else:
            raise TestFailure("host golden mode accepted an image larger than B")


TESTS = [
    test_monitor_command_contract,
    test_xmodem_targets_keep_firmware_rx_on_sd,
    test_update_entry_points_and_trial_reboot,
    test_trial_boot_repairs_primary_before_monitor,
    test_shared_reader_frontends_use_one_update_engine,
    test_running_firmware_selfupdate_callback_boots_verified_trial,
    test_fixed_slots_and_fault_cut_model,
    test_preflight_and_ab_commit_order,
    test_host_golden_mode_validates_manifest_and_uses_selfrx,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except (OSError, TestFailure) as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")

    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} golden self-update tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} golden self-update tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
