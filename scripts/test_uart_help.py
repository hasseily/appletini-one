#!/usr/bin/env python3
"""Keep the '?' UART help in step with the live command parser."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UART = ROOT / "ps_sources" / "frontend" / "uart_control.c"
CORE1 = ROOT / "ps_sources" / "frontend_core1" / "main.c"


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def main() -> int:
    source = UART.read_text(encoding="utf-8")
    core1 = CORE1.read_text(encoding="utf-8")
    help_body = source.split("void uart_control_print_help", 1)[1]
    help_body = help_body.split("static uart_control_event_t", 1)[0]
    parser = source.split("static uart_control_event_t process_command", 1)[1]
    parser = parser.split("uart_control_event_t uart_control_poll", 1)[0]

    commands = set(re.findall(
        r'str_ieq\(argv\[0\],\s*"([^"]+)"\)', parser))
    help_lower = help_body.lower()
    missing = sorted(cmd for cmd in commands
                     if re.search(rf"\b{re.escape(cmd.lower())}\b",
                                  help_lower) is None)
    require(not missing,
            "UART help omits live top-level commands: " + ", ".join(missing))

    for text in (
            "shadow [main|aux|a2li]",
            "disk2 verify",
            "dma dump <mcaddr24>",
            "dma write <addr> [verify]",
            "bustail find <addr16>",
            "sswatch [on|off]",
            "pqw <hex-addr> <hex-hi> <hex-lo>",
            "rtc get | rtc set"):
        require(text in help_body, f"UART help omits {text!r}")

    require("if (c == ':' || c == ';')" in source and
            ": or ; starts a command" in help_body,
            "UART help must list both command-mode prefixes")
    require("shadow: requested A2Li state on UART0" in source,
            "shadow a2li must acknowledge its exact request")
    require("queued = apple_fb_debug_dump_take();" in core1 and
            "s_dump_req = queued;" in core1 and
            "apple_cycle_renderer_debug_a2li_line();" in core1,
            "CPU1 must let a new A2Li request replace an old row dump")

    print(f"PASS UART help covers {len(commands)} live commands")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
