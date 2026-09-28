# Inspect the configured normal-build profile without changing the project.
# Do not source configure_vivado_run_profile.tcl here: this test must detect
# stale settings, not repair them before checking.
set script_dir [file dirname [file normalize [info script]]]
set xpr_path [file join $script_dir .. project appletini_yarz.xpr]

proc require_equal {actual expected message} {
    if {$actual ne $expected} {
        error "$message: expected '$expected', got '$actual'"
    }
}

set threads_hook [file join $script_dir vivado_run_threads.tcl]
set expected_synth [dict create \
    STRATEGY {Vivado Synthesis Defaults} \
    STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE Default \
    STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING auto \
    STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD auto \
    STEPS.SYNTH_DESIGN.TCL.PRE $threads_hook]
set expected_impl [dict create \
    STRATEGY {Vivado Implementation Defaults} \
    STEPS.OPT_DESIGN.IS_ENABLED 1 \
    STEPS.OPT_DESIGN.ARGS.DIRECTIVE Explore \
    STEPS.POWER_OPT_DESIGN.IS_ENABLED 0 \
    STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default \
    STEPS.POST_PLACE_POWER_OPT_DESIGN.IS_ENABLED 0 \
    STEPS.PHYS_OPT_DESIGN.IS_ENABLED 1 \
    STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore \
    STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE Explore \
    STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED 0 \
    STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Default \
    STEPS.INIT_DESIGN.TCL.PRE $threads_hook \
    STEPS.OPT_DESIGN.TCL.PRE [file join $script_dir apply_fabric_timing_margin.tcl] \
    STEPS.ROUTE_DESIGN.TCL.POST [file join $script_dir finish_video_timing.tcl]]

open_project $xpr_path
foreach name {synth_1 impl_1} settings [list $expected_synth $expected_impl] {
    set run [get_runs $name]
    require_equal [llength $run] 1 "Run $name count"
    dict set settings AUTO_INCREMENTAL_CHECKPOINT 0
    dict set settings INCREMENTAL_CHECKPOINT {}
    dict for {property expected} $settings {
        require_equal [get_property $property $run] $expected "$name $property"
        if {[string match {STEPS.*.TCL.*} $property] && ![file isfile $expected]} {
            error "Missing run hook: $expected"
        }
    }
    foreach property [list_property $run] {
        if {([regexp {^STEPS\..*\.TCL\.(PRE|POST)$} $property] ||
             [string match {STEPS.*.ARGS.MORE OPTIONS} $property]) &&
            ![dict exists $settings $property]} {
            require_equal [string trim [get_property $property $run]] {} \
                "$name stale override $property"
        }
    }
}
foreach run [get_runs -filter {IS_SYNTHESIS && SRCSET != sources_1}] {
    require_equal [get_property STEPS.SYNTH_DESIGN.TCL.PRE $run] $threads_hook \
        "$run worker-thread hook"
}
close_project
puts "PASS: timing run properties (read-only)"
