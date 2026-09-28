#!/usr/bin/env python3
"""Check run-profile cleanup, isolation and reuse with a small Tcl model."""

from pathlib import Path
import tempfile
import tkinter


ROOT = Path(__file__).resolve().parents[1]


MODEL = r"""
set runs {}
set writes {}
set threads 2
proc get_runs {args} {
    if {[lindex $args 0] eq "-filter"} {
        if {[lindex $args 1] ne "IS_SYNTHESIS && SRCSET != sources_1"} {
            error "Unexpected run filter: $args"
        }
        set result {}
        dict for {run properties} $::runs {
            if {[dict get $properties IS_SYNTHESIS] &&
                [dict get $properties SRCSET] ne "sources_1"} {
                lappend result $run
            }
        }
        return $result
    }
    if {[llength $args] == 0} {return [dict keys $::runs]}
    return [dict keys $::runs [lindex $args 0]]
}
proc get_property {property run} {
    if {[dict exists $::runs $run $property]} {
        return [dict get $::runs $run $property]
    }
    return ""
}
proc list_property {run} {return [dict keys [dict get $::runs $run]]}
proc set_property {property value run} {
    lappend ::writes [list $run $property $value]
    dict set ::runs $run $property $value
}
proc get_param {name} {
    if {$name ne "general.maxThreads"} {error "Unexpected parameter: $name"}
    return $::threads
}
proc set_param {name value} {
    if {$name ne "general.maxThreads"} {error "Unexpected parameter: $name"}
    set ::threads $value
}
"""


def main() -> int:
    interp = tkinter.Tcl()
    interp.eval(MODEL)
    for run, synth, srcset in (
        ("synth_1", 1, "sources_1"), ("impl_1", 0, "sources_1"),
        ("synth_experiment", 1, "sources_1"),
        ("impl_experiment", 0, "sources_1"), ("ip_synth", 1, "ip_sources"),
    ):
        properties = {"IS_SYNTHESIS": synth, "SRCSET": srcset,
                      "STRATEGY": "Inherited strategy",
                      "AUTO_INCREMENTAL_CHECKPOINT": 1,
                      "INCREMENTAL_CHECKPOINT": "old.dcp"}
        steps = ("SYNTH_DESIGN",) if synth else (
            "INIT_DESIGN", "OPT_DESIGN", "PLACE_DESIGN", "PHYS_OPT_DESIGN",
            "ROUTE_DESIGN", "POST_ROUTE_PHYS_OPT_DESIGN", "WRITE_BITSTREAM")
        for step in steps:
            for suffix, value in (("TCL.PRE", "old_pre.tcl"),
                                  ("TCL.POST", "old_post.tcl"),
                                  ("ARGS.MORE OPTIONS", "-old_option")):
                properties[f"STEPS.{step}.{suffix}"] = value
            if step not in ("INIT_DESIGN", "WRITE_BITSTREAM"):
                properties[f"STEPS.{step}.ARGS.DIRECTIVE"] = "Inherited directive"
        if synth:
            properties["STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING"] = "on"
            properties["STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD"] = "8"
        else:
            for step in ("OPT_DESIGN", "PHYS_OPT_DESIGN"):
                properties[f"STEPS.{step}.IS_ENABLED"] = "0"
            for step in ("POWER_OPT_DESIGN", "POST_PLACE_POWER_OPT_DESIGN",
                         "POST_ROUTE_PHYS_OPT_DESIGN"):
                properties[f"STEPS.{step}.IS_ENABLED"] = "1"
        for property, value in properties.items():
            interp.call("dict", "set", "runs", run, property, value)
    before = {run: interp.call("dict", "get", interp.getvar("runs"), run)
              for run in ("synth_experiment", "impl_experiment", "ip_synth")}

    with tempfile.TemporaryDirectory(prefix="appletini_run_profile_") as tmp:
        directory = Path(tmp)
        for name in ("configure_vivado_run_profile.tcl", "vivado_run_threads.tcl",
                     "apply_fabric_timing_margin.tcl", "finish_video_timing.tcl"):
            (directory / name).write_text((ROOT / "scripts" / name).read_text(encoding="utf-8"), encoding="utf-8")
        profile = directory / "configure_vivado_run_profile.tcl"
        interp.call("source", str(profile))
        assert int(interp.getvar("threads")) == 8

        required = {
            "synth_1": {
                "STRATEGY": "Vivado Synthesis Defaults",
                "STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE": "Default",
                "STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING": "auto",
                "STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD": "auto",
                "STEPS.SYNTH_DESIGN.TCL.PRE": "vivado_run_threads.tcl",
            },
            "impl_1": {
                "STRATEGY": "Vivado Implementation Defaults",
                "STEPS.OPT_DESIGN.ARGS.DIRECTIVE": "Explore",
                "STEPS.PLACE_DESIGN.ARGS.DIRECTIVE": "Default",
                "STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE": "Explore",
                "STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE": "Explore",
                "STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE": "Default",
                "STEPS.INIT_DESIGN.TCL.PRE": "vivado_run_threads.tcl",
                "STEPS.OPT_DESIGN.TCL.PRE": "apply_fabric_timing_margin.tcl",
                "STEPS.ROUTE_DESIGN.TCL.POST": "finish_video_timing.tcl",
            },
        }
        for step, enabled in (("OPT_DESIGN", 1), ("PHYS_OPT_DESIGN", 1),
                              ("POWER_OPT_DESIGN", 0),
                              ("POST_PLACE_POWER_OPT_DESIGN", 0),
                              ("POST_ROUTE_PHYS_OPT_DESIGN", 0)):
            required["impl_1"][f"STEPS.{step}.IS_ENABLED"] = str(enabled)
        for run, settings in required.items():
            settings.update(AUTO_INCREMENTAL_CHECKPOINT="0", INCREMENTAL_CHECKPOINT="")
            for property, expected in settings.items():
                if expected.endswith(".tcl"):
                    expected = (directory / expected).as_posix()
                actual = str(interp.call("get_property", property, run))
                assert actual == expected, (run, property, actual, expected)
            for property in interp.splitlist(interp.call("list_property", run)):
                if property.endswith((".TCL.PRE", ".TCL.POST", ".ARGS.MORE OPTIONS")):
                    if property not in settings:
                        assert not interp.call("get_property", property, run), (run, property)
        for run in ("synth_experiment", "impl_experiment"):
            assert interp.call("dict", "get", interp.getvar("runs"), run) == before[run]
        ip_expected = interp.call("dict", "replace", before["ip_synth"],
                                  "STEPS.SYNTH_DESIGN.TCL.PRE",
                                  (directory / "vivado_run_threads.tcl").as_posix())
        assert interp.call("dict", "get", interp.getvar("runs"), "ip_synth") == ip_expected

        # Only run-property writes invalidate synthesis; set_param is process-local.
        assert int(interp.eval("llength $writes")) > 0
        interp.setvar("writes", "")
        interp.call("source", str(profile))
        assert int(interp.eval("llength $writes")) == 0, interp.getvar("writes")
        assert int(interp.getvar("threads")) == 8

    print("PASS: Vivado profile clears stale settings, isolates experiments, and reapplies without run writes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
