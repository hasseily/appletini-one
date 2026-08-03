#!/usr/bin/env python3
"""Source and build checks for the NMOS-6502 Appletini web demos."""

from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "software" / "appletini_webserver"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def verify_system(name):
    binary = APP / "build" / f"{name}.SYSTEM"
    listing = APP / "build" / f"{name}.lst"
    map_file = APP / "build" / f"{name}.map"
    if not binary.is_file() or not listing.is_file() or not map_file.is_file():
        return False

    require(re.search(r'\.setcpu\s+"6502"',
                      listing.read_text(encoding="utf-8")) is not None,
            f"{name} listing must declare the NMOS 6502 target")
    map_text = map_file.read_text(encoding="utf-8")
    require("timer_init (appletini_timer" in map_text and
            "timer_read (appletini_timer" in map_text,
            f"{name} must resolve IP65 timing to appletini_timer.o")
    require("timer_init (a2_timer.o)" not in map_text and
            "timer_read (a2_timer.o)" not in map_text,
            f"{name} must not link IP65's CPU-speed-dependent a2_timer.o")
    image = binary.read_bytes()
    require(len(image) > 58 and image[:4] == b"\x00\x05\x16\x00",
            f"{name} must carry an AppleSingle header")
    entry_count = struct.unpack_from(">H", image, 24)[0]
    entries = [struct.unpack_from(">III", image, 26 + (12 * i))
               for i in range(entry_count)]
    data_fork = next((entry for entry in entries if entry[0] == 1), None)
    prodos_info = next((entry for entry in entries if entry[0] == 11), None)
    require(data_fork is not None and data_fork[1] == 58 and
            data_fork[2] == len(image) - data_fork[1],
            f"{name} AppleSingle data-fork metadata is stale")
    require(prodos_info is not None and
            image[prodos_info[1]:prodos_info[1] + prodos_info[2]] ==
            b"\x00\xC3\x00\xFF\x00\x00\x20\x00",
            f"{name} must encode ProDOS SYS at load address $2000")
    return True


def main():
    server = (APP / "webserver.c").read_text(encoding="utf-8")
    browser = (APP / "browser.c").read_text(encoding="utf-8")
    image_viewer = (APP / "a2img.c").read_text(encoding="utf-8")
    image_net = (APP / "a2img_net.s").read_text(encoding="utf-8")
    network = (APP / "appletini_net.c").read_text(encoding="utf-8")
    timer = (APP / "appletini_timer.s").read_text(encoding="utf-8")
    build = (APP / "build.bat").read_text(encoding="utf-8")
    disk = (ROOT / "scripts" / "build_appletini_demo_disk.py").read_text(
        encoding="utf-8")
    wave_basic = (ROOT / "software" / "wave_animation.bas").read_text(
        encoding="utf-8")
    wave_code = (ROOT / "software" / "wave_animation_code.a65").read_text(
        encoding="utf-8")

    require("-t apple2" in build and "--cpu 6502" in build and
            "apple2-system.cfg" in build,
            "build must explicitly create NMOS-6502 ProDOS programs")
    require("apple2enh" not in build.lower(),
            "build must not use the enhanced Apple II target")
    for target in ("A2WEBSRV", "A2BROWSE", "A2IMG"):
        require(target in build, f"build must create {target}.SYSTEM")
    require("appletini_net.c" in build and "appletini_timer.s" in build and
            "ip65\\ip65_web.lib" in build,
            "both programs must use the shared W5100/IP65 path and wall-clock timer")
    require(build.rindex("appletini_timer.s") <
            build.rindex("ip65\\ip65_web.lib"),
            "the Appletini timer must precede IP65 so a2_timer.o is not extracted")

    require("RDVBLBAR       = $C019" in timer and
            "MACHINE_ID     = $FBB3" in timer and
            "cpx #IIE_ID" in timer,
            "timer must select real VBL time for the IIe family and vTW")
    require("cmp last_vbl_state" in timer and
            "adc #FRAME_MS" in timer and
            "FRAME_MS       = 17" in timer,
            "timer must advance once per real VBL edge")
    require("jsr MON_WAIT" in timer and "adc #33" in timer,
            "timer must retain the stock 1 MHz II/II+ fallback")

    require("U2_MODE = 0x03U" in network and
            "U2_MODE != 0x03U" in network and "W5100S" not in network,
            "software must probe only the W5100-compatible interface")
    config_load = network[network.index("static void load_card_config"):
                          network.index("static void prepare_macraw")]
    for register in ("W5100_SHAR", "W5100_SIPR", "W5100_SUBR", "W5100_GAR"):
        require(f"w5100_read({register}" in config_load,
                f"saved {register} value must be read from the card")
    require("(appletini_config.mac[0] & 0x01U)" in network and
            "APPLETINI_NET_INVALID_MAC" in network,
            "multicast and empty source MACs must be rejected")
    require("APPLETINI_NET_INVALID_IP" in network and
            "CARD HAS NO VALID SAVED IP CONFIG" in network,
            "empty saved IP configuration must stay visible")

    stack_config = network[network.index("static void apply_stack_config"):
                           network.index("static void apply_card_config")]
    for field in ("cfg_mac", "cfg_ip", "cfg_netmask", "cfg_gateway"):
        require(field in stack_config,
                f"IP65 must receive saved {field} before initialization")
    require("cfg_dns" in stack_config and "appletini_config.gateway" in
            stack_config,
            "browser DNS must use the saved W5100 gateway")
    card_config = network[network.index("static void apply_card_config"):
                          network.index("uint8_t appletini_network_init")]
    for register in ("W5100_SHAR", "W5100_SIPR", "W5100_SUBR", "W5100_GAR"):
        require(f"w5100_write({register}" in card_config,
                f"stack initialization must restore saved {register}")
    init = network[network.index("uint8_t appletini_network_init"):
                   network.index("const char *appletini_network_error")]
    require(init.index("load_card_config();") <
            init.index("apply_stack_config();") <
            init.index("prepare_macraw();") <
            init.index("memcpy(&w5100[4]") <
            init.index("ip65_init(APPLETINI_SLOT)") <
            init.index("apply_card_config();"),
            "shared init must preserve card configuration across MACRAW setup")
    require("w5100_write8(W5100_RMSR, 0x06U)" in network and
            "w5100_write8(W5100_TMSR, 0x06U)" in network,
            "shared init must establish the 4+2+1+1KB W5100 map")

    require("httpd_start(80U, http_server)" in server and
            "static void __fastcall__ http_server" in server,
            "server must retain its IP65 HTTP listener and fastcall ABI")
    require("appletini_network_init()" in server and
            "appletini_network_init()" in browser,
            "both demos must load the card configuration")
    require('#define HOME_URL   "http://frogfind.com/"' in browser and
            '#define PROXY_PRE  "http://frogfind.com/read.php?a="' in browser and
            "url_download(cur_url" in browser,
            "browser must use the current plain-HTTP FrogFind frontend")
    cap = re.search(r"#define BUF_CAP\s+(\d+)U", browser)
    require(cap is not None and int(cap.group(1)) >= 10240,
            "text browser needs the full response/render buffer")
    require("find_body(total, &body)" in browser and
            "render(body, len)" in browser and
            "exit_to_menu();" in browser,
            "browser must decode HTTP bodies and return to the demo menu")
    inverse = browser[browser.index("static void inverse_cputc"):
                      browser.index("static void status_line")]
    require("c >= 'a' && c <= 'z'" in inverse and
            "(uint8_t)c | 0x80U" in inverse and
            inverse.index("revers(0);") < inverse.index("cputc((char") <
            inverse.index("revers(1);"),
            "browser must emit $61-$7A screen codes for inverse lowercase")
    require("inverse_cputs(left);" in browser and
            "inverse_cputs(right);" in browser and
            "inverse_cputc(buf[start]);" in browser and
            "inverse_cputc(c);" in browser,
            "all browser reverse-video text must use safe lowercase output")
    require("auxsrc := $06" in image_net and "auxdst := $08" in image_net and
            "auxlen := $0A" in image_net and
            ".importzp ptr1, ptr2" not in image_net and
            "lda (auxsrc),y" in image_net and "sta (auxdst),y" in image_net,
            "A2IMG aux copies must keep pointers and countdown in private ZP")
    require("lda auxlen" in image_net and "dec auxlen" in image_net and
            "dec _aux_len" not in image_net,
            "A2IMG must not decrement main-memory BSS while RAMWRT is on")
    require("if (sp >= 4095U)" in image_viewer and
            'fail("LZW CHAIN LOOP")' in image_viewer,
            "A2IMG must bound corrupted LZW prefix-chain stack writes")
    require("net_poll();" in image_viewer and
            "NEWVIDEO = 0xC1" in image_viewer and
            "aux_copy(line_buf);" in image_viewer,
            "A2IMG must retain streaming decode directly into SHR memory")
    require('cputs("URL: HTTP://")' in server and
            'cputs("/\\r\\nSUBNET: ")' in server and
            'cputs("\\r\\nGATEWAY: ")' in server,
            "server console must retain the full card configuration")
    stopped = server[server.index('cputs("\\r\\nSERVER STOPPED'):
                     server.index("return 0;", server.index(
                         'cputs("\\r\\nSERVER STOPPED'))]
    require("wait_for_key();" in stopped and "exit_to_menu();" in stopped,
            "server shutdown must remain visible and return to the demo menu")
    require("ip65_diag" not in build and "ip65_ctr" not in network and
            "arp_cache" not in network,
            "the production web demo must exclude packet diagnostics")
    require("A2BROWSE.SYSTEM" in disk and "A2IMG.SYSTEM" in disk and
            "WAVE.ANIMATION" in disk and
            '"WAVE.CODE", "BIN", "0x0300"' in disk,
            "800KB demo disk must include all three web apps and wave assets")
    require("WAVE ANIMATION BY BRENDAN ROBERT" in wave_basic and
            "Wave Animation by Brendan Robert" in wave_code and
            'CHR$(4)"BLOAD WAVE.CODE"' in wave_basic and
            "POKE 769" in wave_basic and "CALL 768" in wave_basic and
            "CALL 792" in wave_basic,
            "wave BASIC driver must load and call the extracted helper")
    require("!cpu 65c02" in wave_code and "* = $0300" in wave_code and
            "wave_copy:" in wave_code and "wave_animate:" in wave_code and
            "jmp ROM_MOVE" in wave_code and "jmp ROM_BASIC" in wave_code,
            "wave helper must preserve both original $0300 entry points")
    require((APP / "ip65" / "LICENSE.txt").is_file() and
            (APP / "ip65" / "LICENSE-W5100.txt").is_file() and
            (APP / "ip65" / "SOURCE.txt").is_file(),
            "vendored IP65 library must retain licenses and provenance")

    built = [verify_system(name)
             for name in ("A2WEBSRV", "A2BROWSE", "A2IMG")]
    print("Appletini 6502 web demo tests passed" +
          ("" if all(built) else " (build not present)"))


if __name__ == "__main__":
    main()
