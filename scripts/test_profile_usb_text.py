#!/usr/bin/env python3
"""Native USB report -> production menu dispatcher -> Save As editor regression."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

from test_usb_gamepad_input import render_production_harness, extract_function
from test_fixed_gamepad_menu import array
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
FRONT=ROOT/'ps_sources/frontend'
OUT=ROOT/'build/profile_usb_text'

def functions(source,names):return '\n'.join(extract_function(source,n)[1] for n in names)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-dispatch',metavar='REV',help='prove the production USB dispatcher from REV loses text')
    args=parser.parse_args()
    compiler=find_native_c_compiler()
    if compiler is None:raise RuntimeError('Native compiler required')
    sources={n:(FRONT/n).read_text(encoding='utf-8') for n in ('usb_hid_service.c','main.c','config_menu.c','config_menu_profiles.c','profile_manager.c')}
    if args.negative_dispatch:
        previous=subprocess.check_output(['git','show',args.negative_dispatch+':ps_sources/frontend/main.c'],cwd=ROOT,text=True,encoding='utf-8')
        sources['main.c']=sources['main.c'].replace(extract_function(sources['main.c'],'ui_handle_usb_menu_event')[1],extract_function(previous,'ui_handle_usb_menu_event')[1])
    usb=render_production_harness(sources['usb_hid_service.c'],boot_keyboard=True)
    usb=usb[:usb.index('#define CHECK')]
    fixture=(ROOT/'scripts/fixtures/profile_usb_text_harness.c').read_text(encoding='utf-8')
    menu=sources['config_menu.c'];profiles=sources['config_menu_profiles.c'];front=sources['main.c']
    definitions='\n'.join(line for line in profiles.splitlines() if line.startswith('#define CONFIG_PROFILE_'))
    tables='\n'.join(array(menu,n) for n in ('k_usb_binding_keys','k_usb_binding_source_order','k_usb_binding_button_source_order'))
    menu_names=('config_menu_is_active','config_menu_onee_fixed_bindings_active','config_menu_set_usb_owned','config_menu_set_usb_bindings_editable',
                'config_menu_usb_binding_capture_action','config_menu_usb_binding_source_valid','config_menu_translate_usb_fixed_action',
                'config_menu_translate_usb_binding_explicit','config_menu_translate_usb_binding','config_menu_set_status','config_menu_basename')
    profile_names=('profiles_copy_text','profile_name_trim_copy','profile_name_char_allowed','profile_name_editor_start','profile_name_editor_cancel',
                   'profile_name_editor_delete','profile_name_editor_append','profile_name_editor_status_fresult','profile_name_editor_commit',
                   'profile_vk_move','profile_vk_select','profile_name_editor_handle_input','config_menu_profiles_handle_input','config_menu_profiles_save_as')
    main_names=('ui_make_input','ui_config_menu_has_close_consumer','ui_input_requests_menu_close','ui_handle_input_with_config',
                'ui_close_config_menu_child','ui_key_from_usb_menu_source','ui_sync_usb_menu_capture','ui_handle_usb_menu_event')
    # Declarations come first because production definitions retain their order/signatures.
    declarations='\n'.join(extract_function(menu,n)[0] for n in menu_names)+'\n'+\
                 '\n'.join(extract_function(profiles,n)[0] for n in profile_names)+'\n'+\
                 '\n'.join(extract_function(front,n)[0] for n in main_names)
    manager=functions(sources['profile_manager.c'],('profile_name_char_valid','profile_manager_profile_name_valid'))
    fixture=fixture.replace('/* DECLARATIONS */',definitions+'\n'+tables+'\n'+declarations)
    fixture=fixture.replace('/* PRODUCTION_MENU */',functions(menu,menu_names)+'\n'+manager+'\n'+functions(profiles,profile_names)+'\n'+functions(front,main_names))
    OUT.mkdir(parents=True,exist_ok=True);(OUT/'include').mkdir(exist_ok=True)
    (OUT/'include/ff.h').write_text('#pragma once\n'+'typedef enum {FR_OK,FR_DISK_ERR,FR_INT_ERR,FR_NOT_READY,FR_NO_FILE,FR_NO_PATH,FR_INVALID_NAME,FR_DENIED,FR_EXIST,FR_INVALID_OBJECT,FR_WRITE_PROTECTED,FR_INVALID_DRIVE,FR_NOT_ENABLED,FR_NO_FILESYSTEM,FR_MKFS_ABORTED,FR_TIMEOUT,FR_LOCKED,FR_NOT_ENOUGH_CORE,FR_TOO_MANY_OPEN_FILES,FR_INVALID_PARAMETER} FRESULT;\n')
    (OUT/'include/xiicps.h').write_text('#ifndef XIICPS_H\n#define XIICPS_H\ntypedef struct { int unused; } XIicPs;\n#endif\n')
    generated=OUT/('negative.c' if args.negative_dispatch else 'profile_usb_text.c')
    generated.write_text(usb+'\n'+fixture)
    executable=generated.with_suffix('.exe')
    cmd=[str(compiler),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(generated),str(FRONT/'usb_controller_identity.c'),
         '-I',str(OUT/'include'),'-I',str(FRONT),'-I',str(ROOT/'scripts/fixtures'),'-I',str(ROOT/'third_party/CherryUSB/class/hid'),'-I',str(ROOT/'third_party/CherryUSB/common'),'-o',str(executable)]
    if 'mingw' in str(compiler).lower():cmd.insert(1,'-static')
    result=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,encoding='utf-8')
    if result.returncode:raise AssertionError('compile failed:\n'+result.stdout+result.stderr)
    result=subprocess.run([str(executable)],cwd=ROOT,capture_output=True,text=True,encoding='utf-8')
    if args.negative_dispatch:
        assert result.returncode and 'text beats conflicting OK/screenshot bindings' in result.stderr,result
        print('PASS negative control: pre-fix production dispatcher fails the typing regression')
    else:
        assert result.returncode==0,result.stdout+result.stderr
        print(result.stdout,end='')
    headers=('usb_hid_service.h','usb_keyboard_ascii.h','usb_gamepad_input.h','onee_usb_controls.h','config_menu.h','config_menu_internal.h','uart_control.h','profile_manager.h')
    report={'native_production_functions':True,'hardware_test':False,'guest_bridge_stubbed':True,
            'filesystem_and_unrelated_menu_pages_stubbed':True,'negative_dispatch_revision':args.negative_dispatch,
            'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr,
            'source_sha256':{n:hashlib.sha256((FRONT/n).read_bytes()).hexdigest() for n in (*sources,*headers)},
            'generated_harness_sha256':hashlib.sha256(generated.read_bytes()).hexdigest()}
    (OUT/('negative.json' if args.negative_dispatch else 'validation.json')).write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
