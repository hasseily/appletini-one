# One fixed profile for create_project and build_and_export_xsa.
# Measured flow: retain the good fabric placement, then repair pixel routes.
namespace eval appletini_run_profile {
    # Preserve completed runs when their settings already match the profile.
    proc set_if_changed {run property expected} {
        if {[get_property $property $run] ne $expected} {
            set_property $property $expected $run
        }
        if {[get_property $property $run] ne $expected} {
            error "Vivado run setting did not apply: $run $property"
        }
    }
    set helper_dir [file dirname [file normalize [info script]]]
    set threads_hook [file join $helper_dir vivado_run_threads.tcl]
    set margin_apply [file join $helper_dir apply_fabric_timing_margin.tcl]
    set route_finish [file join $helper_dir finish_video_timing.tcl]
    foreach hook [list $threads_hook $margin_apply $route_finish] {
        if {![file isfile $hook]} {error "Missing Vivado run hook: $hook"}
    }
    source $threads_hook
    set synth [get_runs synth_1]
    set impl [get_runs impl_1]
    set_if_changed $synth STRATEGY {Vivado Synthesis Defaults}
    set_if_changed $impl STRATEGY {Vivado Implementation Defaults}

    set synth_settings [dict create \
        STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE Default \
        STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING auto \
        STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD auto \
        STEPS.SYNTH_DESIGN.TCL.PRE $threads_hook]
    set impl_settings [dict create \
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
        STEPS.OPT_DESIGN.TCL.PRE $margin_apply \
        STEPS.ROUTE_DESIGN.TCL.POST $route_finish]
    # The route hook repairs only pixel paths and clears both temporary margins.
    foreach run [list $synth $impl] settings [list $synth_settings $impl_settings] {
        # Clear stale hooks/options without clearing a desired hook first.
        foreach property [list_property $run] {
            if {([regexp {^STEPS\..*\.TCL\.(PRE|POST)$} $property] ||
                 [string match {STEPS.*.ARGS.MORE OPTIONS} $property]) &&
                ![dict exists $settings $property]} {
                dict set settings $property {}
            }
        }
        dict set settings AUTO_INCREMENTAL_CHECKPOINT 0
        dict set settings INCREMENTAL_CHECKPOINT {}
        dict for {property expected} $settings {
            set_if_changed $run $property $expected
        }
    }
    # IP/OOC synthesis runs use other source filesets; leave their strategies
    # and post hooks intact. Do not alter other top-level experiment runs.
    foreach run [get_runs -filter {IS_SYNTHESIS && SRCSET != sources_1}] {
        set_if_changed $run STEPS.SYNTH_DESIGN.TCL.PRE $threads_hook
    }
    puts "Vivado profile: OPT Explore, PLACE Default, PHYS Explore, ROUTE Explore, pixel route repair"
}
