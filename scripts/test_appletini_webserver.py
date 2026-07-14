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
    if not binary.is_file() or not listing.is_file():
        return False

    require('.setcpu\t\t"6502"' in listing.read_text(encoding="utf-8"),
            f"{name} listing must declare the NMOS 6502 target")
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
    network = (APP / "appletini_net.c").read_text(encoding="utf-8")
    build = (APP / "build.bat").read_text(encoding="utf-8")
    disk = (ROOT / "scripts" / "build_appletini_demo_disk.py").read_text(
        encoding="utf-8")

    require("-t apple2" in build and "--cpu 6502" in build and
            "apple2-system.cfg" in build,
            "build must explicitly create NMOS-6502 ProDOS programs")
    require("apple2enh" not in build.lower(),
            "build must not use the enhanced Apple II target")
    for target in ("A2WEBSRV", "A2BROWSE"):
        require(target in build, f"build must create {target}.SYSTEM")
    require("appletini_net.c" in build and "ip65\\ip65_web.lib" in build,
            "both programs must use the shared W5100/IP65 path")

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
    require('#define APPLETINI_URL "http://httpbin.io/headers"' in browser and
            "url_download(APPLETINI_URL" in browser,
            "browser must request the exact plain-HTTP demo URL")
    cap = re.search(r"#define RESPONSE_CAP\s+(\d+)U", browser)
    require(cap is not None and int(cap.group(1)) >= 1460,
            "IP65 HTTP client needs at least a full TCP payload buffer")
    require("strstr(response, \"\\r\\n\\r\\n\")" in browser and
            "print_text(body != NULL ? body + 4 : response)" in browser,
            "browser must display the HTTP status and response body")
    require('cputs("URL: HTTP://")' in server and
            'cputs("/\\r\\nSUBNET: ")' in server and
            'cputs("\\r\\nGATEWAY: ")' in server,
            "server console must retain the full card configuration")
    stopped = server[server.index('cputs("\\r\\nSERVER STOPPED'):
                     server.index("return 0;", server.index(
                         'cputs("\\r\\nSERVER STOPPED'))]
    require("wait_for_key();" in stopped and
            "wait_for_key();" in browser,
            "results and startup errors must remain visible")
    require("ip65_diag" not in build and "ip65_ctr" not in network and
            "arp_cache" not in network,
            "the production web demo must exclude packet diagnostics")
    require("A2BROWSE.SYSTEM" in disk and '"3  WEB BROWSER"' in disk,
            "800KB demo disk must include and launch the browser")
    require((APP / "ip65" / "LICENSE.txt").is_file() and
            (APP / "ip65" / "LICENSE-W5100.txt").is_file() and
            (APP / "ip65" / "SOURCE.txt").is_file(),
            "vendored IP65 library must retain licenses and provenance")

    built = [verify_system(name) for name in ("A2WEBSRV", "A2BROWSE")]
    print("Appletini 6502 web demo tests passed" +
          ("" if all(built) else " (build not present)"))


if __name__ == "__main__":
    main()
