#!/usr/bin/env python3
"""Run actual metadata diagnostics and HUD helpers against read-only NOR."""

import os
from pathlib import Path
import shutil
import subprocess

from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/boot-metadata-diagnostics-test"

PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "multiboot_platform.h"
#include "crc32.h"
#include "fw_update_metadata.h"
static uint8_t nor[MB_FLASH_SIZE];
static uint32_t read_calls,read_bytes,write_calls,fail_address=UINT32_MAX;
static unsigned fail_length,platform_available=1;
static int nor_read(void *ctx,uint32_t address,void *dst,uint32_t len)
{
    (void)ctx;
    /* A diagnostic may read only the two metadata records, never payloads. */
    assert((address==MB_METADATA_A_OFFSET || address==MB_METADATA_B_OFFSET) &&
           len<=sizeof(fw_update_metadata_t));
    ++read_calls;read_bytes+=len;
    if(address==fail_address && (!fail_length || len==fail_length)) return -1;
    memcpy(dst,nor+address,len);return 0;
}
static int nor_erase(void *ctx,uint32_t address,uint32_t len)
{ (void)ctx;(void)address;(void)len;++write_calls;assert(!"diagnostic erased flash");return -1; }
static int nor_program(void *ctx,uint32_t address,const void *src,uint32_t len)
{ (void)ctx;(void)address;(void)src;(void)len;++write_calls;assert(!"diagnostic wrote flash");return -1; }
static mb_flash_t flash={NULL,nor_read,nor_erase,nor_program,MB_FLASH_SIZE};
const mb_flash_t *mb_platform_flash(void) { return platform_available ? &flash:NULL; }
static void put32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4U;++i) p[i]=(uint8_t)(value>>(8U*i)); }
static uint32_t checksum(const void *p,uint32_t length)
{ return crc32_finish(crc32_update(crc32_init(),p,length)); }
static void modern_crc(uint32_t address)
{ put32(nor+address+124,checksum(nor+address,124)); }
static void modern(uint32_t address,uint32_t sequence,uint32_t product)
{
    uint8_t *p=nor+address;
    memset(p,0,MB_METADATA_SIZE);memcpy(p,"ATNMBT1\0",8);
    put32(p+8,1);put32(p+12,MB_METADATA_SIZE);put32(p+16,sequence);put32(p+20,product);
    for(unsigned i=0;i<2U;++i) {
        uint32_t family=i ? MB_PRODUCT_MULTITINI:MB_PRODUCT_APPLETINI;
        put32(p+24+i*32,family);put32(p+28+i*32,mb_product_offset(family));
        put32(p+32+i*32,mb_product_capacity(family));
    }
    modern_crc(address);
}
static void legacy(uint32_t address)
{
    fw_update_metadata_t record={0};
    record.magic=FW_UPDATE_METADATA_MAGIC;record.version=FW_UPDATE_METADATA_VERSION;
    record.length_bytes=sizeof(record);record.seq=4;
    record.golden_size=0x200000;record.firmware_offset=MB_APPLETINI_OFFSET;
    record.firmware_size=0xdf0000;record.flags=FW_UPDATE_METADATA_FLAG_FIRMWARE_VERIFIED;
    strcpy(record.golden_version,"Boot B1.2.0");
    record.crc32=checksum(&record,sizeof(record)-4U);
    memcpy(nor+address,&record,sizeof(record));
}
static void legacy_crc(uint32_t address)
{
    put32(nor+address+sizeof(fw_update_metadata_t)-4U,
          checksum(nor+address,sizeof(fw_update_metadata_t)-4U));
}
static void reset(void)
{
    memset(nor,0xff,sizeof nor);read_calls=read_bytes=write_calls=0;
    fail_address=UINT32_MAX;fail_length=0;platform_available=1;
}
'''

SCENARIOS = r'''
static void expect_metadata(unsigned expected)
{
    read_calls=read_bytes=0;
    assert(update_metadata_read_from_qspi()==expected);
    assert(read_bytes<=2U*MB_METADATA_SIZE+sizeof(fw_update_metadata_t));
    assert(!write_calls);
}
static void metadata_diagnostics(void)
{
    reset();modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);
    expect_metadata(1);assert(read_bytes==256 && read_calls==2);
    reset();modern(MB_METADATA_B_OFFSET,2,MB_PRODUCT_MULTITINI);
    expect_metadata(1);assert(read_bytes==256);
    modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);
    expect_metadata(1);assert(read_bytes==256);
    /* A torn/CRC-invalid redundant copy is ignored when the other is valid. */
    nor[MB_METADATA_A_OFFSET+124]^=1;expect_metadata(1);
    nor[MB_METADATA_B_OFFSET+124]^=1;expect_metadata(0);
    reset();expect_metadata(0);assert(read_bytes==424);
    /* Equal sequence with conflicting contents and half-range ordering are
     * ambiguous even when both individual CRCs are valid. */
    modern(MB_METADATA_A_OFFSET,7,MB_PRODUCT_APPLETINI);
    modern(MB_METADATA_B_OFFSET,7,MB_PRODUCT_MULTITINI);expect_metadata(0);
    modern(MB_METADATA_B_OFFSET,0x80000007U,MB_PRODUCT_MULTITINI);expect_metadata(0);
    modern(MB_METADATA_A_OFFSET,0xfffffffeU,MB_PRODUCT_APPLETINI);
    modern(MB_METADATA_B_OFFSET,1,MB_PRODUCT_MULTITINI);expect_metadata(1);
    reset();modern(MB_METADATA_A_OFFSET,0,MB_PRODUCT_APPLETINI);expect_metadata(0);
    modern(MB_METADATA_A_OFFSET,1,0x300);expect_metadata(0);
    modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);
    put32(nor+MB_METADATA_A_OFFSET+28,MB_MULTITINI_OFFSET);modern_crc(MB_METADATA_A_OFFSET);
    expect_metadata(0);
    modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);
    put32(nor+MB_METADATA_A_OFFSET+12,MB_METADATA_SIZE+4);modern_crc(MB_METADATA_A_OFFSET);
    expect_metadata(0);
    modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);
    nor[MB_METADATA_A_OFFSET+92]=1;modern_crc(MB_METADATA_A_OFFSET);expect_metadata(0);
    /* Legacy compatibility is specifically FF0000, not the new FE0000 base. */
    reset();legacy(MB_METADATA_B_OFFSET);expect_metadata(1);assert(read_bytes==424);
    reset();legacy(MB_METADATA_A_OFFSET);expect_metadata(0);
    reset();legacy(MB_METADATA_B_OFFSET);
    nor[MB_METADATA_B_OFFSET+sizeof(fw_update_metadata_t)-4U]^=1;expect_metadata(0);
    legacy(MB_METADATA_B_OFFSET);put32(nor+MB_METADATA_B_OFFSET+4,3);legacy_crc(MB_METADATA_B_OFFSET);expect_metadata(0);
    legacy(MB_METADATA_B_OFFSET);put32(nor+MB_METADATA_B_OFFSET+8,sizeof(fw_update_metadata_t)+4U);legacy_crc(MB_METADATA_B_OFFSET);expect_metadata(0);
    legacy(MB_METADATA_B_OFFSET);put32(nor+MB_METADATA_B_OFFSET,0);legacy_crc(MB_METADATA_B_OFFSET);expect_metadata(0);
    /* Neither a stale UI/Golden cache nor a valid older record can mask a
     * metadata read failure that may hide newer transactional invalidation. */
    reset();modern(MB_METADATA_A_OFFSET,1,MB_PRODUCT_APPLETINI);legacy(MB_METADATA_B_OFFSET);
    fail_address=MB_METADATA_A_OFFSET;expect_metadata(0);assert(read_calls==1);
    fail_address=MB_METADATA_B_OFFSET;expect_metadata(0);assert(read_calls==2);
    reset();legacy(MB_METADATA_B_OFFSET);fail_address=MB_METADATA_B_OFFSET;
    fail_length=sizeof(fw_update_metadata_t);expect_metadata(0);assert(read_calls==3);
    reset();platform_available=0;expect_metadata(0);assert(!read_calls);
    puts("PASS: actual dual/v4 metadata health, redundancy/torn/ambiguous/corrupt/read errors; bounded 256/424-byte read-only diagnostics");
}
'''

HUD_PREAMBLE = r'''
typedef struct {
    uint8_t metadata_valid,usb_storage_attention,boot_device,settings_loaded,session_only,show_bezel;
    char golden_version[32],bezel_status[160];
    uint32_t fb_debug,fb_debug2,temp,rtc;
    struct { uint32_t submit_error_count,transfer_error_count;int last_error; } hid_status;
} debug_overlay_snapshot_t;
#define HUD_LEFT_X 8
#define HUD_LEFT_Y 148
#define HUD_LEFT_W 304
#define HUD_V(n) (n)
#define HUD_MUTED 1U
#define HUD_WARN 2U
#define FB16_COLOR_WHITE 3U
static char golden_line[192];static uint32_t golden_color;
static const char *boot_device_name(uint8_t device) { (void)device;return "SmartPort"; }
static void format_temp(char *text,size_t size,const void *sensor) { (void)sensor;snprintf(text,size,"temperature"); }
static void format_rtc(char *text,size_t size,const void *sensor) { (void)sensor;snprintf(text,size,"RTC"); }
static void panel(uint16_t *fb,int x,int y,int w,int h,const char *title)
{ (void)fb;(void)x;(void)y;(void)w;(void)h;assert(!strcmp(title,"System")); }
static void line(uint16_t *fb,int x,int y,int w,unsigned index,const char *text,uint32_t color)
{
    (void)fb;(void)x;(void)y;(void)w;
    if(index==2U) { snprintf(golden_line,sizeof golden_line,"%s",text);golden_color=color; }
}
'''

HUD_SCENARIOS = r'''
static void hud_diagnostics(void)
{
    debug_overlay_snapshot_t s={0};char warnings[192];
    strcpy(s.golden_version,"B2.0.5");
    build_warnings(&s,warnings,sizeof warnings);
    assert(strstr(warnings,"boot metadata invalid"));
    draw_system(NULL,&s);assert(!strcmp(golden_line,"Golden B2.0.5") && golden_color==HUD_MUTED);
    s.metadata_valid=1;build_warnings(&s,warnings,sizeof warnings);assert(!warnings[0]);
    draw_system(NULL,&s);assert(!strcmp(golden_line,"Golden B2.0.5") && golden_color==HUD_MUTED);
    strcpy(s.golden_version,"Unknown");draw_system(NULL,&s);
    assert(!strcmp(golden_line,"Golden Unknown") && golden_color==HUD_WARN);
    s.golden_version[0]=0;draw_system(NULL,&s);
    assert(!strcmp(golden_line,"Golden Unknown") && golden_color==HUD_WARN);
    s.metadata_valid=0;s.fb_debug=8;s.usb_storage_attention=1;
    build_warnings(&s,warnings,sizeof warnings);
    assert(strstr(warnings,"boot metadata invalid") && strstr(warnings,"framebuffer AXI errors") &&
           strstr(warnings,"USB0 storage attention"));
    puts("PASS: actual HUD retains invalid-metadata warning, independently shows installed Golden and warns on Unknown");
}
int main(void) { metadata_diagnostics();hud_diagnostics();return 0; }
'''


def main() -> None:
    runtime = (ROOT / "ps_sources/frontend/main.c").read_text()
    overlay = (ROOT / "ps_sources/frontend/debug_overlay.c").read_text()
    helper = function(runtime, "update_metadata_read_from_qspi")
    assert "0xFC000000" not in helper and "APPLETINI_FLASH_METADATA_OFFSET" not in helper
    assert "g_multiboot_status_valid" not in helper
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / "diagnostics.c"
    executable = OUT / "diagnostics.exe"
    harness.write_text(
        PREAMBLE + helper + SCENARIOS + HUD_PREAMBLE +
        function(overlay, "append_warning") + function(overlay, "build_warnings") +
        function(overlay, "draw_system") + HUD_SCENARIOS
    )
    compiler = shutil.which("gcc") or "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
        "-I", str(ROOT / "ps_sources"), "-I", str(ROOT / "ps_sources/lib"),
        str(harness), str(ROOT / "ps_sources/lib/multiboot.c"),
        str(ROOT / "ps_sources/lib/crc32.c"), "-o", str(executable),
    ], check=True, env=env)
    subprocess.run([str(executable)], check=True, env=env)


if __name__ == "__main__":
    main()
