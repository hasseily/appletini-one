#!/usr/bin/env python3
"""Native state-machine test for ONE//e menu pause and input ownership."""

from __future__ import annotations

import shutil
import subprocess
import textwrap
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
OUT_DIR = ROOT / "build" / "onee_ui_policy_native"


def find_compiler() -> Path:
    for name in ("gcc", "cc", "clang"):
        found = shutil.which(name)
        if found:
            return Path(found)
    for path in sorted(
        Path("C:/Xilinx").glob(
            "*/tps/mingw/*/win64.o/nt/bin/x86_64-w64-mingw32-gcc.exe"
        ),
        reverse=True,
    ):
        return path
    raise FileNotFoundError("no host C compiler found")


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stdout)
    return completed.stdout


def main() -> int:
    source = (FRONTEND / "onee_service.c").read_text(encoding="utf-8")
    frontend = (FRONTEND / "main.c").read_text(encoding="utf-8")
    if "g_ui_menu_paused" not in source or \
            "g_ui_input_release_wait" not in source or \
            "g_onee_menu_paused" in frontend:
        raise AssertionError("ONE//e service does not own the UI policy state")

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    harness = OUT_DIR / "onee_ui_policy_harness.c"
    executable = OUT_DIR / "onee_ui_policy_harness.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>

        static uint32_t mode_status;
        static uint8_t runtime_live;
        static uint8_t paused;
        static uint8_t released;
        static uint8_t refuse_unpause;
        static uint8_t fixed_mode;
        static uint8_t blocked;

        static uint32_t test_reg_read(uint32_t address)
        {
            (void)address;
            return mode_status;
        }

        static void test_reg_write(uint32_t address, uint32_t value)
        {
            (void)address;
            (void)value;
        }

        #define COMMON_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "onee_service.c"

        static uint32_t status_base(void)
        {
            return (CARD_CTRL_ONEE_STATUS_SIGNATURE <<
                    CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
                   CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
        }

        static uint32_t status_safe_off(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint32_t status_effective(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                   CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                   CARD_CTRL_ONEE_STATUS_SELECTED_BIT |
                   CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT;
        }

        static uint32_t status_activity(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
                   CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_APPLE_ACTIVITY <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint8_t runtime_start(void *ctx)
        {
            (void)ctx;
            runtime_live = 1U;
            return 1U;
        }

        static void runtime_suspend(void *ctx)
        {
            (void)ctx;
            runtime_live = 0U;
        }

        static void runtime_stop(void *ctx)
        {
            (void)ctx;
            runtime_live = 0U;
        }

        static uint8_t runtime_running(void *ctx)
        {
            (void)ctx;
            return runtime_live;
        }

        static uint8_t set_paused(void *ctx, uint8_t value)
        {
            (void)ctx;
            if (value == 0U && refuse_unpause != 0U) {
                return 0U;
            }
            paused = value;
            return 1U;
        }

        static void set_input_policy(void *ctx,
                                     uint8_t new_fixed_mode,
                                     uint8_t new_blocked)
        {
            (void)ctx;
            fixed_mode = new_fixed_mode;
            blocked = new_blocked;
        }

        static uint8_t input_released(void *ctx)
        {
            (void)ctx;
            return released;
        }

        static int check(int condition, const char *message)
        {
            if (!condition) {
                fprintf(stderr, "FAIL: %s\n", message);
                return 0;
            }
            return 1;
        }

        int main(void)
        {
            mode_status = status_safe_off();
            onee_service_init();
            onee_service_bind_runtime(runtime_start, runtime_suspend,
                                      runtime_stop, runtime_running, NULL);
            onee_service_bind_ui_policy(set_paused, set_input_policy,
                                        input_released, NULL);
            if (!check(fixed_mode == 0U && blocked == 0U,
                       "ordinary host mode did not publish open input")) {
                return 1;
            }

            if (!check(onee_service_request_start() != 0U,
                       "safe manual start was refused")) {
                return 1;
            }
            mode_status = status_effective();
            onee_service_poll();
            onee_service_sync_menu_policy(0U);
            if (!check(fixed_mode != 0U && blocked == 0U && paused == 0U,
                       "running mode did not select fixed unblocked input")) {
                return 1;
            }

            onee_service_sync_menu_policy(1U);
            if (!check(paused != 0U && blocked != 0U &&
                       onee_service_menu_paused() != 0U,
                       "open menu did not pause and block ONE//e input")) {
                return 1;
            }

            released = 0U;
            onee_service_sync_menu_policy(0U);
            if (!check(paused != 0U && blocked != 0U &&
                       onee_service_input_release_wait() != 0U,
                       "menu close did not wait for input release")) {
                return 1;
            }

            released = 1U;
            refuse_unpause = 1U;
            onee_service_sync_menu_policy(0U);
            if (!check(paused != 0U && blocked != 0U,
                       "failed resume released input too early")) {
                return 1;
            }
            refuse_unpause = 0U;
            onee_service_sync_menu_policy(0U);
            if (!check(paused == 0U && blocked == 0U && fixed_mode != 0U &&
                       onee_service_input_release_wait() == 0U,
                       "released input did not resume the selected machine")) {
                return 1;
            }

            onee_service_sync_menu_policy(1U);
            mode_status = status_activity();
            onee_service_poll();
            onee_service_sync_menu_policy(1U);
            if (!check(paused == 0U && fixed_mode == 0U && blocked == 0U,
                       "safety stop did not clear the whole UI policy")) {
                return 1;
            }

            puts("ONEE UI POLICY NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    compiler = find_compiler()
    command = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(harness), "-o", str(executable), f"-I{FRONTEND}",
    ]
    if "mingw" in compiler.as_posix().lower():
        command.insert(5, "-static")
    run(command)
    output = run([str(executable)])
    if "ONEE UI POLICY NATIVE PASS" not in output:
        raise AssertionError("native harness did not report success")
    print("PASS: ONE//e service owns menu pause and input release")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
