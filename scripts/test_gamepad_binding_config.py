#!/usr/bin/env python3
"""Execute the real binding text parser and serializer for old and new IDs."""

import os
from pathlib import Path
import re
import shutil
import subprocess
from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    front = ROOT / "ps_sources/frontend"
    menu = (front / "config_menu.c").read_text()
    hid = (front / "usb_hid_service.c").read_text()
    out = ROOT / "build/gamepad_binding_config"
    out.mkdir(parents=True, exist_ok=True)
    code = '''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb_hid_service.h"
#define HID_KBD_USAGE_ERRUNDEF 3U
#define HID_KBD_USAGE_MAX 0xE7U
'''
    code += "\n".join(re.findall(r"^#define CONFIG_USB_KEY_USAGE_.*$", menu, re.M))
    code += "\n#define CONFIG_USB_KEY_SOURCE(usage) (USB_HID_MENU_SOURCE_KEY_BASE | (usage))\n"
    for name in ("usb_hid_menu_source_from_keyboard_usage", "usb_hid_menu_source_is_keyboard",
                 "usb_hid_menu_source_from_gamepad_button", "usb_hid_menu_source_is_gamepad"):
        code += function(hid, name)
    for name in ("config_menu_ascii_lower", "config_menu_ascii_upper", "config_menu_str_ieq",
                 "config_menu_str_starts_ieq", "config_menu_usb_key_usage_config",
                 "config_menu_usb_binding_source_config", "config_menu_usb_key_usage_value",
                 "config_menu_usb_binding_source_value"):
        code += function(menu, name)
    code += r'''
int main(void) {
    char text[64];
    for (unsigned n=1;n<=32;++n) {
        snprintf(text,sizeof(text),"gamepad.button%u",n);
        const unsigned source=config_menu_usb_binding_source_value(text);
        assert(source==0x200+n-1);
        assert(usb_hid_menu_source_is_gamepad(source));
        assert(!usb_hid_menu_source_is_keyboard(source));
        assert(config_menu_usb_binding_source_value(
            config_menu_usb_binding_source_config(source))==source);
    }
    const char *bad[]={"GAMEPAD.BUTTON0","GAMEPAD.BUTTON33","GAMEPAD.BUTTON-1",
        "GAMEPAD.BUTTON","GAMEPAD.BUTTON1garbage","GAMEPAD.BUTTON4294967296"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
        assert(config_menu_usb_binding_source_value(bad[i])==USB_HID_MENU_SOURCE_NONE);
    for (unsigned usage=4;usage<=0xE7;++usage) {
        const unsigned source=USB_HID_MENU_SOURCE_KEY_BASE|usage;
        assert(config_menu_usb_binding_source_value(
            config_menu_usb_binding_source_config(source))==source);
    }
    const char *old[]={"MOUSE.LEFT","MOUSE.RIGHT","MOUSE.MIDDLE","MOUSE.BUTTON4",
        "MOUSE.BUTTON5","WHEEL.UP","WHEEL.DOWN","NONE"};
    for (unsigned i=0;i<sizeof(old)/sizeof(old[0]);++i) {
        const unsigned source=config_menu_usb_binding_source_value(old[i]);
        assert(strcmp(old[i],config_menu_usb_binding_source_config(source))==0);
    }
    puts("PASS: 32 gamepad bindings, malformed IDs, 228 keyboard IDs, legacy mouse/wheel round trips");
    return 0;
}
'''
    source = out / "test.c"
    source.write_text(code, encoding="utf-8")
    compiler = shutil.which("gcc") or shutil.which("clang") or "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    exe = out / "test.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(front),
                    str(source), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
