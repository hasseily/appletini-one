#!/usr/bin/env python3
"""Native main-loop update authorization/queue regressions; no network or flash."""
from pathlib import Path
import hashlib
import json
import subprocess

from test_usb_gamepad_input import extract_function
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / 'ps_sources/frontend'
OUT = ROOT / 'build/online_update_main'


def main():
    runtime = (FRONT / 'main.c').read_text(encoding='utf-8')
    menu = (FRONT / 'config_menu.c').read_text(encoding='utf-8')
    loop = runtime[runtime.index('    while (1) {', runtime.index('int main(void)')):]
    # Boot takeover skips the remaining iteration. The actual safety poll must
    # run before that branch so it can close ownership rather than be starved.
    assert loop.index('control_service_online_update(&ui, &config_menu);') < \
        loop.index('if (vtw_service_boot_start_pending() != 0U)'), \
        'pending boot takeover must not skip update cancellation'
    assert loop.index('control_service_online_update(&ui, &config_menu);') < \
        loop.index('if (vtw_service_boot_handoff_pending() != 0U)'), \
        'boot handoff must receive update safety cancellation first'
    names = ('menu_platform_request_golden_boot_update',
             'card_control_apply_slot_policy', 'menu_platform_ethernet_update_busy',
             'menu_platform_ethernet_read_config', 'menu_platform_ethernet_write_config',
             'menu_platform_ethernet_test', 'menu_platform_ethernet_dhcp_start',
             'menu_platform_ethernet_dhcp_poll', 'menu_platform_ethernet_dhcp_cancel',
             'menu_platform_get_online_update_status', 'menu_platform_request_online_update_check',
             'menu_platform_request_online_update_download', 'menu_platform_cancel_online_update',
             'menu_platform_request_online_update_install', 'ticks_to_us',
             'control_online_update_allowed', 'control_online_update_busy',
             'control_service_online_update', 'ui_online_update_composite_due')
    menu_names = ('config_menu_poll_online_update', 'config_menu_online_update_finished',
                  'config_menu_ethernet_card_access')
    fixture = (ROOT / 'scripts/fixtures/online_update_main_harness.c').read_text(encoding='utf-8')
    bodies = '\n'.join(extract_function(menu, name)[1] for name in menu_names)
    bodies += '\n' + '\n'.join(extract_function(runtime, name)[1] for name in names)
    generated = fixture.replace('/* PRODUCTION_FUNCTIONS */', bodies)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'xiicps.h').write_text('#pragma once\ntypedef struct {int unused;} XIicPs;\n', encoding='utf-8')
    harness = OUT / 'online_update_main.c'
    harness.write_text(generated, encoding='utf-8')
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError('Native compiler required')
    command = [str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(OUT), '-I', str(FRONT), str(harness), '-o', str(OUT / 'test.exe')]
    if 'mingw' in str(compiler).lower():
        command.insert(1, '-static')
    compiled = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding='utf-8')
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    completed = subprocess.run([str(OUT / 'test.exe')], cwd=OUT, capture_output=True, text=True, encoding='utf-8')
    report = {
        'hardware_test': False, 'production_main_functions': names,
        'production_menu_functions': menu_names,
        'online_service_storage_http_and_flash_mocked': True,
        'scope': 'authorization, polling gates and deferred installer queues; not flash execution',
        'main_loop_source_order': 'update safety poll before boot-start early continue and handoff branch',
        'scheduling_checks': 'maximum8 foreground passes, cooperative2000us budget, only safe maintenance HTTP pumping,10Hz progress paints with immediate input/stage/error/terminal/full-refresh',
        'source_sha256': {name: hashlib.sha256((FRONT / name).read_bytes()).hexdigest()
                          for name in ('main.c', 'config_menu.c', 'config_menu.h', 'online_update_core.h')},
        'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest(),
        'exit_code': completed.returncode, 'stdout': completed.stdout, 'stderr': completed.stderr,
    }
    (OUT / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    assert completed.returncode == 0, completed.stdout + completed.stderr
    print(completed.stdout, end='')


if __name__ == '__main__':
    main()
