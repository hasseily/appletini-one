#!/usr/bin/env python3
"""Check timing-hook rejection and exact restoration with a small Tcl model."""

from pathlib import Path
import tkinter


ROOT = Path(__file__).resolve().parents[1]
APPLY = ROOT / "scripts/apply_fabric_timing_margin.tcl"
CLEAR = ROOT / "scripts/clear_fabric_timing_margin.tcl"
FINISH = ROOT / "scripts/finish_video_timing.tcl"
CONFIG = ROOT / "scripts/fabric_timing_margin.tcl"
MODEL = r"""
set model_margin 0.2
set uncertainty 0.0
set pixel_uncertainty 0.0
set dir_limit 10.0
set phi_limit 8.0
set missing_port ""
set missing_path 0
set missing_clock ""
set duplicate_clock ""
set missing_clock_path ""
set duplicate_clock_path ""
set ignore_clock_apply ""
set ignore_clock_restore ""
set ignore_apply 0
set ignore_restore 0
set writes {}
set clock_writes {}
set repairs {}
set fail_repair 0
proc get_clocks {args} {
    switch [lindex $args end] {
        clk_out1_zynq_ps_bd_clk_wiz_1_0 {set result fabric_clock}
        clk_out1_zynq_ps_bd_clk_wiz_0_0 {set result pixel_clock}
        default {error "Unexpected clock selector: $args"}
    }
    if {$result eq $::missing_clock} {return {}}
    if {$result eq $::duplicate_clock} {return [list $result $result]}
    return $result
}
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
    if {$from eq $to} {
        if {$from eq $::missing_clock_path} {return {}}
        if {$from eq $::duplicate_clock_path} {return [list $from $from]}
        return $from
    }
    if {$::missing_path} {return {}}
    if {$from eq "a2fpga_clk"} {return phi_path}
    return dir_path
}
proc get_property {name path} {
    if {$name eq "USER_UNCERTAINTY"} {
        if {$path eq "pixel_clock"} {return $::pixel_uncertainty}
        if {$path eq "fabric_clock"} {return $::uncertainty}
        error "Unexpected clock path: $path"
    }
    if {$name ne "REQUIREMENT"} {error "Unexpected property $name"}
    if {$path eq "phi_path"} {return $::phi_limit}
    return $::dir_limit
}
proc set_clock_uncertainty {args} {
    lappend ::clock_writes $args
    set value [lindex $args 1]
    foreach clock [lindex $args end] {
        if {($value == $::model_margin && $clock eq $::ignore_clock_apply) ||
            ($value == 0.0 && $clock eq $::ignore_clock_restore)} {continue}
        switch $clock {
            fabric_clock {set ::uncertainty $value}
            pixel_clock {set ::pixel_uncertainty $value}
            default {error "Unexpected uncertainty target: $clock"}
        }
    }
}
proc set_max_delay {args} {
    lappend ::writes $args
    set datapath [expr {[lindex $args 0] eq "-datapath_only"}]
    set limit [lindex $args $datapath]
    if {($::ignore_apply && ($limit == 10.0 - $::model_margin || $limit == 8.0 - $::model_margin)) ||
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
proc phys_opt_design {args} {
    if {$args ne {-directive AggressiveExplore}} {
        error "Unexpected routed repair options: $args"
    }
    if {$::uncertainty != $::model_margin || $::pixel_uncertainty != $::model_margin ||
        $::dir_limit != 10.0 - $::model_margin || $::phi_limit != 8.0 - $::model_margin} {
        error "Routed repair ran without both margins and tightened board limits"
    }
    lappend ::repairs $args
    if {$::fail_repair} {error "Modeled repair failure"}
}
"""


def model(margin: str | None = None) -> tkinter.Tcl:
    interp = tkinter.Tcl()
    interp.eval(MODEL)
    interp.eval("unset -nocomplain ::env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)")
    if margin is not None:
        interp.setvar("env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)", margin)
        interp.setvar("model_margin", float(margin))
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


def require_nominal(interp: tkinter.Tcl) -> None:
    assert float(interp.eval("set uncertainty")) == 0.0
    assert float(interp.eval("set pixel_uncertainty")) == 0.0
    assert float(interp.eval("set dir_limit")) == 10.0
    assert float(interp.eval("set phi_limit")) == 8.0


def main() -> int:
    for margin in (None, "0.200", "0.400", "1.000"):
        interp = model(margin)
        source(interp, CONFIG)
        assert float(interp.call("appletini_timing_margin::value")) == (0.2 if margin is None else float(margin))
    for value in ("", "NaN", "Inf", "-Inf", "0.199", "1.001", "0.4001", "0.4 ns", "0.4; exit"):
        interp = model()
        interp.setvar("env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)", value)
        expect_failure(interp, APPLY, "APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS")
        assert int(interp.eval("llength $writes")) == 0
        assert int(interp.eval("llength $clock_writes")) == 0

    # Plain apply/clear remains valid for fabric-only experiments and the
    # checkpoint round trip. The normal finish hook performs exactly one repair.
    for final_hook in (CLEAR, FINISH):
        interp = model()
        source(interp, APPLY)
        assert float(interp.eval("set uncertainty")) == 0.2
        assert float(interp.eval("set pixel_uncertainty")) == 0.0
        assert float(interp.eval("set dir_limit")) == 9.8
        assert float(interp.eval("set phi_limit")) == 7.8
        assert interp.eval("lindex $clock_writes 0") == "-setup 0.200 fabric_clock"
        source(interp, final_hook)
        require_nominal(interp)
        assert int(interp.eval("llength $writes")) == 4
        expected_repairs = int(final_hook == FINISH)
        assert int(interp.eval("llength $repairs")) == expected_repairs
        expect_failure(interp, FINISH, "Expected 0.200 ns fabric_clock")
        assert int(interp.eval("llength $repairs")) == expected_repairs

    for variable, value, message in [
        ("missing_port", "a2fpga_dir_a", "Expected both Apple direction ports"),
        ("missing_path", "1", "No timing path"),
        ("dir_limit", "11.0", "Expected 10.000 ns"),
        ("phi_limit", "9.0", "Expected 8.000 ns"),
        ("uncertainty", "0.1", "Refusing to replace fabric_clock user uncertainty"),
        ("pixel_uncertainty", "0.1", "Refusing to replace pixel_clock user uncertainty"),
        ("missing_clock", "fabric_clock", "Expected exactly one fabric clock"),
        ("missing_clock", "pixel_clock", "Expected exactly one fabric clock"),
        ("duplicate_clock", "fabric_clock", "Expected exactly one fabric clock"),
        ("duplicate_clock", "pixel_clock", "Expected exactly one fabric clock"),
        ("missing_clock_path", "fabric_clock", "No setup path for fabric_clock"),
        ("missing_clock_path", "pixel_clock", "No setup path for pixel_clock"),
        ("duplicate_clock_path", "fabric_clock", "No setup path for fabric_clock"),
        ("duplicate_clock_path", "pixel_clock", "No setup path for pixel_clock"),
    ]:
        interp = model()
        interp.setvar(variable, value)
        expect_failure(interp, APPLY, message)
        assert int(interp.eval("llength $writes")) == 0
        assert int(interp.eval("llength $clock_writes")) == 0

    for clock, hook in (("fabric_clock", APPLY), ("pixel_clock", FINISH)):
        interp = model()
        if hook == FINISH:
            source(interp, APPLY)
        interp.setvar("ignore_clock_apply", clock)
        expect_failure(interp, hook, f"{clock} implementation margin did not apply")
        assert int(interp.eval("llength $repairs")) == 0

        interp = model()
        source(interp, APPLY)
        interp.setvar("ignore_clock_restore", clock)
        expect_failure(interp, FINISH, f"{clock} implementation margin did not clear")
        assert int(interp.eval("llength $repairs")) == 1

        for final_hook in (CLEAR, FINISH):
            for variable, message in (("missing_clock_path", f"No setup path for {clock}"),
                                      ("duplicate_clock_path", f"No setup path for {clock}"),
                                      ("missing_clock", "Expected exactly one fabric clock"),
                                      ("duplicate_clock", "Expected exactly one fabric clock")):
                interp = model()
                source(interp, APPLY)
                interp.setvar(variable, clock)
                expect_failure(interp, final_hook, message)
                assert int(interp.eval("llength $writes")) == 2
                assert int(interp.eval("llength $repairs")) == 0

        interp = model()
        source(interp, APPLY)
        interp.setvar("pixel_uncertainty" if clock == "pixel_clock" else "uncertainty", "0.1")
        expected = "0.000 or 0.200" if clock == "pixel_clock" else "0.200"
        expect_failure(interp, CLEAR, f"Expected {expected} ns {clock} implementation margin")
        assert int(interp.eval("llength $writes")) == 2

    # Finish must not overwrite a pre-existing pixel margin, even one that
    # clear would accept. A failed repair must also prevent clearing/export.
    for pixel_value in ("0.1", "0.2"):
        interp = model()
        source(interp, APPLY)
        interp.setvar("pixel_uncertainty", pixel_value)
        expect_failure(interp, FINISH, "Expected 0.000 ns pixel_clock")
        assert int(interp.eval("llength $clock_writes")) == 1
        assert int(interp.eval("llength $repairs")) == 0
    interp = model()
    source(interp, APPLY)
    interp.setvar("fail_repair", "1")
    expect_failure(interp, FINISH, "Modeled repair failure")
    assert float(interp.eval("set uncertainty")) == 0.2
    assert float(interp.eval("set pixel_uncertainty")) == 0.2
    assert int(interp.eval("llength $writes")) == 2

    interp = model()
    interp.setvar("ignore_apply", "1")
    expect_failure(interp, APPLY, "Expected 9.800 ns")

    interp = model()
    source(interp, APPLY)
    interp.setvar("ignore_restore", "1")
    expect_failure(interp, CLEAR, "Expected 10.000 ns")

    expect_failure(model(), CLEAR, "Expected 9.800 ns")
    for margin in ("0.200", "0.400"):
        for final_hook in (CLEAR, FINISH):
            interp = model(margin)
            source(interp, APPLY)
            assert float(interp.eval("set uncertainty")) == float(margin)
            assert float(interp.eval("set pixel_uncertainty")) == 0.0
            assert float(interp.eval("set dir_limit")) == 10.0 - float(margin)
            assert float(interp.eval("set phi_limit")) == 8.0 - float(margin)
            source(interp, final_hook)
            require_nominal(interp)
            assert int(interp.eval("llength $repairs")) == int(final_hook == FINISH)
        for clock in ("fabric_clock", "pixel_clock"):
            interp = model(margin)
            if clock == "pixel_clock":
                source(interp, APPLY)
            interp.setvar("ignore_clock_apply", clock)
            expect_failure(interp, APPLY if clock == "fabric_clock" else FINISH,
                           f"{clock} implementation margin did not apply")
            assert int(interp.eval("llength $repairs")) == 0
            interp = model(margin)
            source(interp, APPLY)
            interp.setvar("ignore_clock_restore", clock)
            expect_failure(interp, FINISH, f"{clock} implementation margin did not clear")
        interp = model(margin)
        source(interp, APPLY)
        interp.setvar("env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)", "0.400" if margin == "0.200" else "0.200")
        expect_failure(interp, FINISH, "before routed timing repair")
        assert int(interp.eval("llength $repairs")) == 0
    print("PASS: default/selected margins, finite configuration, fabric-first timing, one routed repair, exact nominal restoration and failure gates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
