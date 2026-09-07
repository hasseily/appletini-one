#!/usr/bin/env python3
"""Check timing-hook rejection and exact restoration with a small Tcl model."""

from pathlib import Path
import tkinter


ROOT = Path(__file__).resolve().parents[1]
APPLY = ROOT / "scripts/apply_fabric_timing_margin.tcl"
CLEAR = ROOT / "scripts/clear_fabric_timing_margin.tcl"
MODEL = r"""
set uncertainty 0.0
set dir_limit 10.0
set phi_limit 8.0
set missing_port ""
set missing_path 0
set ignore_apply 0
set ignore_restore 0
set writes {}
proc get_clocks {args} {return fabric_clock}
proc get_ports {args} {
    set result {}
    foreach port [lindex $args end] {
        if {$port ne $::missing_port} {lappend result $port}
    }
    return $result
}
proc get_timing_paths {args} {
    set from [lindex $args [expr {[lsearch -exact $args -from] + 1}]]
    set to [lindex $args [expr {[lsearch -exact $args -to] + 1}]]
    if {$from eq $to} {return fabric_path}
    if {$::missing_path} {return {}}
    if {$from eq "a2fpga_clk"} {return phi_path}
    return dir_path
}
proc get_property {name path} {
    if {$name eq "USER_UNCERTAINTY"} {return $::uncertainty}
    if {$name ne "REQUIREMENT"} {error "Unexpected property $name"}
    if {$path eq "phi_path"} {return $::phi_limit}
    return $::dir_limit
}
proc set_clock_uncertainty {args} {set ::uncertainty [lindex $args 1]}
proc set_max_delay {args} {
    lappend ::writes $args
    set datapath [expr {[lindex $args 0] eq "-datapath_only"}]
    set limit [lindex $args $datapath]
    if {($::ignore_apply && $limit in {9.800 7.800}) ||
        ($::ignore_restore && $limit in {10.000 8.000})} {return}
    if {$datapath} {
        if {[lindex $args end] ne "a2fpga_dir_d" ||
            [lindex $args 3] ne "a2fpga_clk"} {
            error "PHI0 constraint selector changed"
        }
        set ::phi_limit $limit
    } else {
        if {[lindex $args end] ne "a2fpga_dir_a a2fpga_dir_d"} {
            error "Direction constraint selector changed"
        }
        set ::dir_limit $limit
    }
}
"""


def model() -> tkinter.Tcl:
    interp = tkinter.Tcl()
    interp.eval(MODEL)
    return interp


def source(interp: tkinter.Tcl, script: Path) -> None:
    interp.call("source", str(script))


def expect_failure(interp: tkinter.Tcl, script: Path, expected: str) -> None:
    try:
        source(interp, script)
    except tkinter.TclError as exc:
        if expected not in str(exc):
            raise AssertionError(f"Unexpected rejection: {exc}") from exc
    else:
        raise AssertionError(f"{script.name} accepted {expected}")


def main() -> int:
    interp = model()
    source(interp, APPLY)
    assert float(interp.eval("set uncertainty")) == 0.2
    assert float(interp.eval("set dir_limit")) == 9.8
    assert float(interp.eval("set phi_limit")) == 7.8
    source(interp, CLEAR)
    assert float(interp.eval("set uncertainty")) == 0.0
    assert float(interp.eval("set dir_limit")) == 10.0
    assert float(interp.eval("set phi_limit")) == 8.0
    assert int(interp.eval("llength $writes")) == 4

    for variable, value, message in [
        ("missing_port", "a2fpga_dir_a", "Expected both Apple direction ports"),
        ("missing_path", "1", "No timing path"),
        ("dir_limit", "11.0", "Expected 10.000 ns"),
        ("phi_limit", "9.0", "Expected 8.000 ns"),
        ("uncertainty", "0.1", "Refusing to replace fabric user uncertainty"),
    ]:
        interp = model()
        interp.setvar(variable, value)
        expect_failure(interp, APPLY, message)
        assert int(interp.eval("llength $writes")) == 0

    interp = model()
    interp.setvar("ignore_apply", "1")
    expect_failure(interp, APPLY, "Expected 9.800 ns")

    interp = model()
    source(interp, APPLY)
    interp.setvar("ignore_restore", "1")
    expect_failure(interp, CLEAR, "Expected 10.000 ns")

    expect_failure(model(), CLEAR, "Expected 9.800 ns")
    print("PASS: timing margin hooks restore nominal limits and reject bad state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
