#!/usr/bin/env python3
"""Native integration of the ONE//e supervisor, vTW runtime and menu pause.

Use the same fixture scaffolding as the vTW tests, but derive ENABLE_EFF and
CORE_RUN from the retained RTL gate and copy the real embedded ROM. This checks
firmware register ordering; it does not emulate the 65C02 or prove board video.
"""

from __future__ import annotations

import ast
import subprocess
import sys
import textwrap
from pathlib import Path

import test_onee_vtw_runtime as runtime

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "onee_startup_native"


def fixture_source() -> str:
    tree = ast.parse(Path(runtime.__file__).read_text(encoding="utf-8"))
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name == "run_native_speed_control_test")
    for node in ast.walk(function):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "harness"
                and node.func.attr == "write_text" and node.args
                and isinstance(node.args[0], ast.Call)
                and node.args[0].args
                and isinstance(node.args[0].args[0], ast.Constant)):
            return textwrap.dedent(ast.literal_eval(node.args[0].args[0]))
    raise AssertionError("vTW native fixture source not found")


def main() -> int:
    compiler = runtime.find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("native C compiler required")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "xiltimer.h").write_text(textwrap.dedent("""
        #ifndef XILTIMER_H
        #define XILTIMER_H
        #include <stdint.h>
        typedef uint64_t XTime;
        #define COUNTS_PER_SECOND 100000000ULL
        void XTime_GetTime(XTime *value);
        #endif
    """), encoding="utf-8")
    source = fixture_source()
    source = source.replace(
        'const uint8_t apple2e_cpu_rom[16384] = { 0U };',
        '#include "../../ps_sources/frontend/apple2e_cpu_rom_data.c"\n'
        '#include "../../ps_sources/frontend/onee_service.c"')
    source = source.replace("static XTime fake_time;", textwrap.dedent(r'''
        static XTime fake_time;
        static uint8_t supervisor_model;
        static uint8_t guard_settles;
        static uint8_t hazard_during_rom;
        static uint8_t input_released;
        static uint8_t input_fixed;
        static uint8_t input_blocked;
        static uint32_t shadow_pointer;
        static uint32_t shadow_writes;
        static uint8_t shadow[0x24000U];
        static uint8_t unsafe_shadow_write;
    '''))
    source = source.replace(
        "return registers[reg_index(address)];", textwrap.dedent(r'''
        if (supervisor_model && address == CARD_CTRL_VTW_STATUS_REG) {
            const uint32_t ctrl = registers[reg_index(CARD_CTRL_VTW_CTRL_REG)];
            const uint32_t mode = registers[reg_index(CARD_CTRL_ONEE_MODE_REG)];
            const uint8_t enable = (ctrl & CARD_CTRL_VTW_CTRL_ENABLE_BIT) &&
                ((mode & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) ||
                 (!(mode & CARD_CTRL_ONEE_STATUS_ISOLATED_BIT) &&
                  boot_menu_service_host_bus_master_allowed()));
            return (enable ? CARD_CTRL_VTW_STATUS_ENABLE_EFF : 0U) |
                ((enable && (ctrl & CARD_CTRL_VTW_CTRL_CORE_RUN_BIT)) ?
                 CARD_CTRL_VTW_STATUS_CORE_RUN : 0U);
        }
        return registers[reg_index(address)];
    '''), 1)
    source = source.replace(
        "if (address != CARD_CTRL_VTW_CTRL_REG || ctrl_write_sticks != 0U) {",
        textwrap.dedent(r'''
        if (supervisor_model && address == CARD_CTRL_ONEE_MODE_REG) {
            uint32_t mode = registers[reg_index(address)];
            const uint32_t session = CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                CARD_CTRL_ONEE_STATUS_SELECTED_BIT;
            if (!(value & CARD_CTRL_ONEE_CTRL_REQUEST_BIT)) {
                mode &= ~session;
            } else {
                mode |= CARD_CTRL_ONEE_STATUS_REQUEST_BIT;
                if (guard_settles) mode |= session;
                mode &= ~(CARD_CTRL_ONEE_STATUS_INHIBIT_MASK <<
                          CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
            }
            registers[reg_index(address)] = mode;
            return;
        }
        if (supervisor_model && address == CARD_CTRL_VTW_SHADOW_ADDR_REG)
            shadow_pointer = value;
        if (supervisor_model && address == CARD_CTRL_VTW_SHADOW_DATA_REG) {
            const uint32_t mode = registers[reg_index(CARD_CTRL_ONEE_MODE_REG)];
            const uint32_t ctrl = registers[reg_index(CARD_CTRL_VTW_CTRL_REG)];
            if (!(mode & CARD_CTRL_ONEE_STATUS_ISOLATED_BIT) ||
                !(mode & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) ||
                !(ctrl & CARD_CTRL_VTW_CTRL_APPLE_RES_BIT) ||
                (ctrl & CARD_CTRL_VTW_CTRL_CORE_RUN_BIT))
                unsafe_shadow_write = 1U;
            if (shadow_pointer < sizeof(shadow)) shadow[shadow_pointer++] = value;
            ++shadow_writes;
            if (hazard_during_rom && shadow_writes == 19U)
                registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] |=
                    CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
                    CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT;
        }
        if (address != CARD_CTRL_VTW_CTRL_REG || ctrl_write_sticks != 0U) {
    '''), 1)
    source = source.replace("int main(void)", "int vtw_only_main(void)", 1)
    source += textwrap.dedent(r'''
        static uint32_t safe_guard(void)
        {
            return (CARD_CTRL_ONEE_STATUS_SIGNATURE <<
                    CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
                CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT |
                CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT |
                CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT |
                (CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF <<
                 CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }
        static uint8_t start_runtime(void *ctx)
        { (void)ctx; return vtw_service_onee_start(0U); }
        static void suspend_runtime(void *ctx)
        { (void)ctx; vtw_service_onee_suspend(); }
        static void stop_runtime(void *ctx)
        { (void)ctx; vtw_service_onee_stop(); }
        static uint8_t runtime_running(void *ctx)
        { (void)ctx; return vtw_service_onee_running(); }
        static uint8_t set_paused(void *ctx, uint8_t pause)
        { (void)ctx; return vtw_service_onee_set_paused(pause); }
        static uint8_t released(void *ctx)
        { (void)ctx; return input_released; }
        static void input_policy(void *ctx, uint8_t fixed, uint8_t blocked)
        { (void)ctx; input_fixed = fixed; input_blocked = blocked; }
        static void bind_runtime(void)
        {
            onee_service_bind_runtime(start_runtime, suspend_runtime,
                                      stop_runtime, runtime_running, NULL);
            onee_service_bind_ui_policy(set_paused, input_policy, released, NULL);
        }
        static void setup(void)
        {
            supervisor_model = 0U;
            reset_fixture();
            supervisor_model = guard_settles = input_released = 1U;
            hazard_during_rom = input_fixed = input_blocked = 0U;
            shadow_pointer = shadow_writes = 0U;
            unsafe_shadow_write = 0U;
            memset(shadow, 0xA5, sizeof(shadow));
            machine_mode = CARD_MACHINE_MODE_UNKNOWN;
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] = safe_guard();
            onee_service_init();
        }
        static int check_started(uint8_t paused)
        {
            return check(onee_service_state() == ONEE_SERVICE_STATE_RUNNING &&
                         !stub_boot_gate_armed &&
                         onee_service_isolation_confirmed() &&
                         vtw_service_onee_running() && shadow_writes == 16386U &&
                         !unsafe_shadow_write && shadow[0x3F3U] == 0U &&
                         shadow[0x3F4U] == 0U &&
                         !memcmp(shadow + VTW_SHADOW_ROM_PHYS,
                                 apple2e_cpu_rom, VTW_ROM_BYTES) &&
                         boot_patch_count == 1U &&
                         boot_patch_reset_held_count == 1U &&
                         !!(first_core_run_ctrl & CARD_CTRL_VTW_CTRL_PAUSE_BIT) == paused,
                         "ONE//e did not cold-load and release the isolated private CPU");
        }
        int main(void)
        {
            /* Saved intent can arrive before runtime bindings, as at config bind. */
            setup();
            /* Saved host vTW ON may coexist with saved ONE//e ON. Its native
             * MID gate must clear before the private machine executes it. */
            vtw_service_set_enabled(1U);
            vtw_service_prepare_boot_start();
            if (!check(stub_boot_gate_armed, "saved host gate not armed")) return 1;
            onee_service_restore_persisted(1U);
            onee_service_poll();
            if (!check(onee_service_restore_pending() &&
                       !g_manual_request && !shadow_writes,
                       "unbound restore started a core")) return 1;
            bind_runtime();
            onee_service_poll();
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] &=
                ~CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT;
            onee_service_poll();
            if (!check_started(0U)) return 1;
            /* An old saved file on late SD arrival cannot replace a live request. */
            onee_service_restore_persisted(0U);
            onee_service_poll();
            if (!check(g_sessions_started == 1U && !onee_service_restore_pending() &&
                       onee_service_state() == ONEE_SERVICE_STATE_RUNNING,
                       "SD retry restarted or stopped ONE//e")) return 1;

            /* Manual menu start runs paused from its first CORE_RUN write. */
            setup(); bind_runtime(); onee_service_sync_menu_policy(1U);
            if (!check(onee_service_request_start(), "safe manual start refused")) return 1;
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] &=
                ~CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT;
            onee_service_poll(); onee_service_sync_menu_policy(1U);
            if (!check_started(1U)) return 1;
            input_released = 0U; onee_service_sync_menu_policy(0U);
            if (!check(vtw_service_onee_paused() && input_blocked &&
                       onee_service_input_release_wait(), "menu exit skipped release wait")) return 1;
            input_released = 1U; onee_service_sync_menu_policy(0U);
            if (!check(!vtw_service_onee_paused() && !input_blocked && input_fixed &&
                       g_sessions_started == 1U, "menu exit did not resume same core")) return 1;

            /* Missing guard keeps ON pending; live Apple hazards cannot start. */
            setup(); bind_runtime(); onee_service_restore_persisted(1U);
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] = 0U;
            for (unsigned i = 0U; i < 300U; ++i) onee_service_poll();
            if (!check(onee_service_restore_pending() && !shadow_writes && !g_manual_request,
                       "missing PL bypassed isolation")) return 1;
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] = safe_guard() |
                CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT | CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT;
            machine_mode = CARD_MACHINE_MODE_IIGS;
            for (unsigned i = 0U; i < ONEE_RESTORE_HAZARD_GRACE_POLLS; ++i)
                onee_service_poll();
            if (!check(!g_manual_request && !shadow_writes && !g_persisted_intent,
                       "physical Apple hazard did not revoke restore")) return 1;

            /* CTRL transport failure stays isolated and retries without a new request. */
            setup(); bind_runtime(); onee_service_restore_persisted(1U);
            onee_service_poll();
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] &=
                ~CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT;
            ctrl_write_sticks = 0U;
            onee_service_poll();
            if (!check(!g_runtime_started && g_manual_request && g_persisted_intent &&
                       !vtw_service_onee_running(), "failed start erased mode choice")) return 1;
            ctrl_write_sticks = 1U;
            shadow_writes = 0U; unsafe_shadow_write = 0U;
            boot_patch_count = boot_patch_reset_held_count = 0U;
            for (unsigned i = 0U; i < ONEE_RUNTIME_RETRY_POLL_LIMIT + 1U; ++i)
                onee_service_poll();
            if (!check_started(0U)) return 1;

            /* A connector event during ROM copy fails closed before CORE_RUN. */
            setup(); bind_runtime(); onee_service_restore_persisted(1U);
            onee_service_poll();
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] &=
                ~CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT;
            hazard_during_rom = 1U; onee_service_poll();
            if (!check(!first_core_run_ctrl && !g_runtime_started && !ctrl_value() &&
                       shadow_writes < 16386U, "mid-copy hazard released the core")) return 1;
            onee_service_poll();
            if (!check(!g_manual_request && !onee_service_restore_pending() &&
                       !vtw_service_onee_running(), "hazard allowed automatic restart")) return 1;
            puts("ONEE STARTUP INTEGRATION PASS (saved/manual, menu, SD retry, guard and faults)");
            return 0;
        }
    ''')
    harness = BUILD / "onee_startup_harness.c"
    executable = BUILD / "onee_startup_harness.exe"
    harness.write_text(source, encoding="utf-8")
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               str(harness), "-o", str(executable), f"-I{BUILD}",
               f"-I{runtime.FRONTEND}", f"-I{ROOT / 'ps_sources' / 'lib'}"]
    if "mingw" in compiler.as_posix().lower():
        command.insert(5, "-static")
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)
    if "--prove-startup-pause-regression" in sys.argv[1:]:
        service = (runtime.ONEE_C).read_text(encoding="utf-8")
        before, marker, after = service.partition(
            "if ((g_status & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) != 0U) {")
        if not marker or "onee_service_update_ui_policy();" not in after:
            raise AssertionError("startup policy call not found")
        service = before + marker + after.replace("onee_service_update_ui_policy();", "", 1)
        service = service.replace('#include "../lib/common.h"', "")
        mutant = BUILD / "onee_startup_old_policy.c"
        mutant_exe = BUILD / "onee_startup_old_policy.exe"
        mutant.write_text(source.replace(
            '#include "../../ps_sources/frontend/onee_service.c"', service),
            encoding="utf-8")
        mutant_command = [str(mutant) if part == str(harness) else
                          str(mutant_exe) if part == str(executable) else part
                          for part in command]
        subprocess.run(mutant_command, cwd=ROOT, check=True)
        failed = subprocess.run([str(mutant_exe)], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if failed.returncode == 0 or "did not cold-load and release" not in failed.stdout:
            raise AssertionError("old unpaused first release was not rejected")
        print("PASS: old startup pause ordering rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
