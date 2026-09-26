#!/usr/bin/env python3
"""Run physical-host USB joystick programs through the production vTW CPU."""
import test_vtw


def main() -> int:
    test_vtw.OUT_DIR = test_vtw.ROOT / "build" / "vtw_usb_joystick_sim"
    test_vtw.SOURCES = [source for source in test_vtw.SOURCES
                       if not source.startswith("hdl/sim/")]
    test_vtw.SOURCES.append("hdl/sim/tb_vtw_usb_joystick.sv")
    test_vtw.BENCHES = [("tb_vtw_usb_joystick", "VTW USB JOYSTICK PASS")]
    return test_vtw.main()


if __name__ == "__main__":
    raise SystemExit(main())
