#!/usr/bin/env python3
"""Run TURBO execution, coherency, and Disk II regression benches.

Uses the same production source set and static integration checks as
test_vtw.py. The CPU instruction differential bench has its own launcher.
"""

from __future__ import annotations

import test_vtw


def main() -> int:
    test_vtw.OUT_DIR = test_vtw.ROOT / "build" / "vtw_turbo_sim"
    focused = {
        "tb_vtw_turbo",
        "tb_vtw_disk2_speed_matrix",
        "tb_vtw_disk2_woz_e2e",
        "tb_smartport_shortcut",
        "tb_vtw_shadow_host_port",
        "tb_vtw_system",
        "tb_vtw_slowdown",
    }
    test_vtw.BENCHES = [entry for entry in test_vtw.BENCHES
                       if entry[0] in focused]
    return test_vtw.main()


if __name__ == "__main__":
    raise SystemExit(main())
