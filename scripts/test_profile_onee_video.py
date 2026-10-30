#!/usr/bin/env python3
"""Native profile file roundtrips through production serialization, parser and apply.

FatFs is an in-memory file adapter. Unrelated settings/device helpers are stubs;
the complete ONE//e-owning functions are extracted unchanged from production.
No hardware or HDL simulation is used. Optional --negative-config REV proves
the regression against an explicit older production config_menu.c revision.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

from test_usb_gamepad_input import extract_function
from test_fixed_gamepad_menu import array
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / 'ps_sources/frontend'
OUT = ROOT / 'build/profile_onee_video'
CORE = (
    'config_menu_save_settings_to_path', 'config_menu_save_profile_settings',
    'config_menu_load_profile_settings', 'config_menu_read_settings_from_path',
    'config_menu_reset_settings_only', 'config_menu_parse_key_value',
    'config_menu_apply_runtime', 'config_menu_apply_runtime_internal',
    'config_menu_apply_boot_runtime_internal', 'config_menu_onee_video_standard_text',
)
PURE = (
    'config_menu_parse_config_line', 'config_menu_str_ieq', 'config_menu_bool_text',
    'config_menu_appendf', 'config_menu_copy_text', 'config_menu_basename',
    'config_menu_on_off', 'config_menu_format_mac', 'config_menu_format_ipv4',
)
MANUAL = {
    'config_menu_open_path', 'config_menu_commit_global_cfg',
    'config_menu_set_sd_error', 'config_menu_set_status',
}


def calls(body):
    return set(re.findall(r'\b(config_menu_\w+)\s*\(', body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-config', metavar='REV')
    args = parser.parse_args()
    source = (FRONT / 'config_menu.c').read_text(encoding='utf-8')
    if args.negative_config:
        source = subprocess.check_output(
            ['git', 'show', args.negative_config + ':ps_sources/frontend/config_menu.c'],
            cwd=ROOT, text=True, encoding='utf-8')
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError('Native compiler required')
    real = dict((name, extract_function(source, name)) for name in CORE)
    pending = list(PURE)
    # Pure text helpers retain their actual helper dependencies, too.
    while pending:
        name = pending.pop()
        if name in real:
            continue
        real[name] = extract_function(source, name)
        pending.extend(calls(real[name][1]) - set(real) - MANUAL)
    dependencies = set().union(*(calls(body) for _, body in real.values()))
    mocked = sorted(dependencies - set(real) - MANUAL)
    internal = (FRONT / 'config_menu_internal.h').read_text(encoding='utf-8')
    stubs = []
    declarations = [prototype for prototype, _ in real.values()]
    for name in mocked:
        try:
            prototype, _ = extract_function(source, name)
        except (ValueError, AssertionError):
            match = re.search(r'(?m)^([\w\s*]+\b' + name + r'\([^;]+\);)', internal)
            if match is None:
                raise RuntimeError('No unrelated-helper declaration: ' + name)
            prototype = match.group(1).strip()
        declarations.append(prototype)
        result_type = prototype.split(name)[0].replace('static', '').strip()
        result = '' if result_type == 'void' else (
            'return "STUB";' if '*' in result_type else
            'return 1;' if name == 'config_menu_phasor_append_settings' else 'return 0;')
        if name == 'config_menu_joystick_config_line':
            result = "if (size != 0) line[0] = '\\0';"
        stubs.append(prototype.rstrip(';') + '\n{ ' + result + ' }')
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define ') and not line.endswith('\\')
                          and not line.startswith('#define APPEND_CFG'))
    constants += '\n' + '\n'.join(re.findall(
        r'typedef enum \{[^}]*\} config_boot_\w+;', source))
    tables = '\n'.join(array(source, name) for name in
                       ('k_browser_lastdir_keys', 'k_usb_binding_config_keys'))
    fixture = (ROOT / 'scripts/fixtures/profile_onee_video_harness.c').read_text(encoding='utf-8')
    generated = fixture.replace('/* CONSTANTS */', constants + '\n' + tables)
    generated = generated.replace('/* DECLARATIONS */', '\n'.join(declarations))
    generated = generated.replace('/* UNRELATED_HELPER_STUBS */', '\n'.join(stubs))
    generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(body for _, body in real.values()))
    OUT.mkdir(parents=True, exist_ok=True)
    includes = OUT / 'include'
    includes.mkdir(exist_ok=True)
    (includes / 'ff.h').write_text(
        '#pragma once\ntypedef unsigned int UINT; typedef unsigned char BYTE;\n'
        'typedef enum {FR_OK,FR_DISK_ERR,FR_INT_ERR,FR_NOT_READY,FR_NO_FILE,'
        'FR_NO_PATH,FR_INVALID_NAME,FR_DENIED,FR_EXIST,FR_INVALID_OBJECT,'
        'FR_WRITE_PROTECTED,FR_INVALID_DRIVE,FR_NOT_ENABLED,FR_NO_FILESYSTEM,'
        'FR_MKFS_ABORTED,FR_TIMEOUT,FR_LOCKED,FR_NOT_ENOUGH_CORE,'
        'FR_TOO_MANY_OPEN_FILES,FR_INVALID_PARAMETER} FRESULT;\n'
        'typedef struct {unsigned slot; unsigned position;} FIL;\n'
        '#define FA_READ 1U\n#define FA_WRITE 2U\n#define FA_CREATE_ALWAYS 8U\n',
        encoding='utf-8')
    (includes / 'xiicps.h').write_text('#pragma once\ntypedef struct {int unused;} XIicPs;\n', encoding='utf-8')
    stem = 'negative' if args.negative_config else 'profile_onee_video'
    harness, exe = OUT / (stem + '.c'), OUT / (stem + '.exe')
    harness.write_text(generated, encoding='utf-8')
    command = [str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
               '-I', str(includes), '-I', str(FRONT), str(harness), '-o', str(exe)]
    if 'mingw' in str(compiler).lower():
        command.insert(1, '-static')
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding='utf-8')
    if result.returncode:
        raise RuntimeError('Compile failed:\n' + result.stdout + result.stderr)
    result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, encoding='utf-8')
    if args.negative_config:
        assert result.returncode and 'PAL serialized into profile' in result.stderr, result
        print('PASS negative control: old production serializer loses profile PAL')
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end='')
    report = {
        'hardware_test': False, 'production_functions': sorted(real),
        'unrelated_helpers_stubbed': mocked,
        'in_memory_fatfs_adapter': True, 'negative_config_revision': args.negative_config,
        'config_menu_source_text_sha256': hashlib.sha256(source.encode('utf-8')).hexdigest(),
        'owning_header_sha256': {name: hashlib.sha256((FRONT / name).read_bytes()).hexdigest()
                                for name in ('config_menu.h', 'config_menu_internal.h')},
        'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest(),
        'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
    }
    (OUT / (stem + '.json')).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
