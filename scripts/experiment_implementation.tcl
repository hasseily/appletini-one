# A new run with explicit defaults; never resets synth_1/impl_1 or promotes
# their replacement. Usage: vivado -mode batch -source this_file -tclargs LABEL
# Optional second argument reuses a current synthesis run made by this script.
# Optional environment knobs: APPLETINI_EXPERIMENT_{OPT,PLACE,PHYS,ROUTE,POST},
# THREADS (1..8), MARGIN_NS (0 or 0.2), UNPIN (0/1). PHYS/POST default to None.
if {$argc < 1 || $argc > 2} {error "Expected LABEL and optional SYNTH_RUN"}
set label [lindex $argv 0]
if {![regexp {^[a-z0-9_-]+$} $label]} {error "Invalid experiment label"}
set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir timing_run_helpers.tcl]
proc setting {name fallback} {
    set key APPLETINI_EXPERIMENT_$name
    if {[info exists ::env($key)]} {return $::env($key)}
    return $fallback
}
set options [dict create opt [setting OPT Default] place [setting PLACE Default] \
    phys [setting PHYS None] route [setting ROUTE Default] post [setting POST None] \
    threads [setting THREADS 8] margin [setting MARGIN_NS 0.0] unpin [setting UNPIN 1]]
if {[dict get $options unpin] ni {0 1}} {error "UNPIN must be 0 or 1"}
set info [timing_run::new_build simplify-$label]
set out [file normalize [dict get $info run_dir]]
set manifest [file join $out manifest.txt]
dict set info experiment_label $label
dict set info options $options
dict set info target_wns_ns 0.050
dict set info vivado_version [version -short]
timing_run::write_manifest $manifest $info
puts "EXPERIMENT_DIR=$out"
set ::env(XILINX_TCLAPP_REPO) [file join $::env(XILINX_VIVADO) data XilinxTclStore]
set ::env(XILINX_LOCAL_USER_DATA) NO
source [file join $script_dir vivado_experiment_threads.tcl]
try {
    open_project project/appletini_yarz.xpr
    source [file join $script_dir register_video_cdc_constraints.tcl]
    set part [get_property PART [current_project]]
    if {$part ne "xc7z020clg484-2"} {error "Unexpected device part: $part"}
    dict set info device_part $part
    set suffix [string map {- _} [dict get $info build_id]]
    if {$argc == 2} {
        set synth_name [lindex $argv 1]
        set sr [get_runs -quiet $synth_name]
        if {![string match synth_simplify_* $synth_name] || [llength $sr] != 1 ||
            [get_property STATUS $sr] ne "synth_design Complete!" ||
            [get_property NEEDS_REFRESH $sr]} {
            error "Reuse requires a complete, current synth_simplify run."
        }
        dict set info synthesis_reused 1
    } else {
        set synth_name synth_simplify_$suffix
        create_run $synth_name -flow [get_property FLOW [get_runs synth_1]] \
            -strategy {Vivado Synthesis Defaults} -constrset constrs_1
        set sr [get_runs $synth_name]
        set_property AUTO_INCREMENTAL_CHECKPOINT 0 $sr
        set_property INCREMENTAL_CHECKPOINT {} $sr
        set_property STEPS.SYNTH_DESIGN.TCL.PRE \
            [file join $script_dir vivado_experiment_threads.tcl] $sr
        dict set info synthesis_reused 0
    }
    set impl_name impl_simplify_$suffix
    create_run $impl_name -parent_run $synth_name \
        -flow [get_property FLOW [get_runs impl_1]] \
        -strategy {Vivado Implementation Defaults} -constrset constrs_1
    set ir [get_runs $impl_name]
    set_property AUTO_INCREMENTAL_CHECKPOINT 0 $ir
    set_property INCREMENTAL_CHECKPOINT {} $ir
    foreach property [list_property $ir] {
        if {[regexp {^STEPS\..*\.TCL\.(PRE|POST)$} $property] ||
            [string match {STEPS.*.ARGS.MORE OPTIONS} $property]} {
            set_property $property {} $ir
        }
    }
    foreach {step key} {OPT_DESIGN opt PLACE_DESIGN place ROUTE_DESIGN route} {
        set_property STEPS.$step.ARGS.DIRECTIVE [dict get $options $key] $ir
    }
    foreach {step key} {PHYS_OPT_DESIGN phys POST_ROUTE_PHYS_OPT_DESIGN post} {
        set directive [dict get $options $key]
        set_property STEPS.$step.IS_ENABLED [expr {$directive ne "None"}] $ir
        set_property STEPS.$step.ARGS.DIRECTIVE \
            [expr {$directive eq "None" ? "Default" : $directive}] $ir
    }
    set_property STEPS.INIT_DESIGN.TCL.PRE \
        [file join $script_dir vivado_experiment_threads.tcl] $ir
    set_property STEPS.OPT_DESIGN.TCL.PRE \
        [file join $script_dir vivado_experiment_constraints.tcl] $ir
    dict set info synthesis_run $synth_name
    dict set info implementation_run $impl_name
    dict set info xdc_sha256 [timing_run::sha256_file hdl/constraints/appletini_yarz.xdc]
    dict set info video_cdc_xdc_sha256 [timing_run::sha256_file hdl/constraints/video_cdc_impl.xdc]
    report_property -all $sr -file [file join $out synthesis_properties.txt]
    report_property -all $ir -file [file join $out implementation_properties.txt]
    timing_run::write_manifest $manifest $info
    puts "SYNTHESIS_RUN=$synth_name"
    puts "IMPLEMENTATION_RUN=$impl_name"
    if {![dict get $info synthesis_reused]} {
        set start [clock seconds]
        launch_runs $synth_name -jobs 8
        wait_on_run $synth_name
        dict set info synthesis_wall_seconds [expr {[clock seconds] - $start}]
        if {[get_property STATUS $sr] ne "synth_design Complete!"} {error "Synthesis failed"}
    }
    dict set info synthesis_dcp_sha256 [timing_run::sha256_file \
        [file join [get_property DIRECTORY $sr] appletini_yarz_top.dcp]]
    set start [clock seconds]
    set final_step [expr {[dict get $options post] eq "None" ? \
        "route_design" : "post_route_phys_opt_design"}]
    launch_runs $impl_name -to_step $final_step -jobs 8
    wait_on_run $impl_name
    dict set info implementation_wall_seconds [expr {[clock seconds] - $start}]
    set implementation_status [get_property STATUS $ir]
    puts "Implementation status: $implementation_status"
    if {$implementation_status ni [list "$final_step Complete!" "$final_step Complete, Failed Timing!"]} {
        error "Implementation did not finish $final_step: $implementation_status"
    }
    open_run $impl_name
    foreach name {clk_out1_zynq_ps_bd_clk_wiz_1_0 clk_out1_zynq_ps_bd_clk_wiz_0_0 \
                  audio_mclk audio_bclk psram_clk_out dvi_clk_out} {
        if {[llength [get_clocks -quiet $name]] != 1} {error "Missing required clock: $name"}
    }
    set fabric [get_clocks clk_out1_zynq_ps_bd_clk_wiz_1_0]
    if {[dict get $options margin] > 0.0} {
        source [file join $script_dir clear_fabric_timing_margin.tcl]
    }
    foreach {domain clock_name} {fabric clk_out1_zynq_ps_bd_clk_wiz_1_0
                                pixel clk_out1_zynq_ps_bd_clk_wiz_0_0} {
        set domain_clock [get_clocks $clock_name]
        set path [get_timing_paths -quiet -from $domain_clock -to $domain_clock -max_paths 1]
        if {[llength $path] != 1} {error "Missing $domain setup path"}
        set uncertainty [get_property USER_UNCERTAINTY $path]
        if {$uncertainty eq ""} {set uncertainty 0.0}
        if {![string is double -strict $uncertainty] || abs($uncertainty) > 0.0005} {
            error "Temporary $domain uncertainty remains: $uncertainty"
        }
        dict set info final_${domain}_user_uncertainty_ns $uncertainty
    }
    foreach spec {{a2fpga_dir_a fabric 10.0} {a2fpga_dir_d fabric 10.0} \
                  {a2fpga_dir_d phi0 8.0}} {
        lassign $spec port domain expected
        set from [expr {$domain eq "fabric" ? $fabric : [get_ports a2fpga_clk]}]
        set path [get_timing_paths -quiet -from $from -to [get_ports $port] -max_paths 1]
        if {[llength $path] != 1 || abs([get_property REQUIREMENT $path] - $expected) > 0.0005} {
            error "Changed board requirement: $domain to $port"
        }
    }
    source [file join $script_dir check_video_bus_constraints.tcl]
    dict set info constraint_bounds_status PASS
    report_timing_summary -max_paths 20 -report_unconstrained -warn_on_violation \
        -file [file join $out timing_summary.rpt]
    report_route_status -file [file join $out route_status.rpt]
    report_bus_skew -warn_on_violation -file [file join $out bus_skew.rpt]
    check_timing -verbose -file [file join $out check_timing.rpt]
    report_methodology -file [file join $out methodology.rpt]
    report_clock_interaction -file [file join $out clock_interaction.rpt]
    report_utilization -file [file join $out utilization.rpt]
    report_control_sets -file [file join $out control_sets.rpt]
    report_design_analysis -congestion -file [file join $out congestion.rpt]
    report_high_fanout_nets -timing -load_types -max_nets 30 \
        -file [file join $out high_fanout.rpt]
    foreach {parser name} {parse_timing_summary timing_summary parse_route_status route_status \
                           parse_bus_skew bus_skew parse_utilization utilization} {
        set info [dict merge $info [timing_run::$parser \
            [timing_run::read_text [file join $out $name.rpt]]]]
    }
    foreach key {wns_ns tns_ns whs_ns ths_ns wpws_ns tpws_ns \
                 setup_failing_endpoints hold_failing_endpoints \
                 pulse_width_failing_endpoints unconstrained_internal_endpoints \
                 route_errors bus_skew_wns_ns} {
        if {![string is double -strict [dict get $info $key]]} {
            error "Missing or invalid result: $key"
        }
    }
    set missing 0
    foreach run [list $sr $ir] {
        set log [file join [get_property DIRECTORY $run] runme.log]
        incr missing [timing_run::count_missing_constraint_objects [timing_run::read_text $log]]
        file copy $log [file join $out [get_property NAME $run].log]
    }
    dict set info missing_constraint_objects $missing
    write_checkpoint -force [file join $out final.dcp]
    set qualifies [expr {[dict get $info wns_ns] >= 0.050 &&
        [dict get $info whs_ns] >= 0.0 && [dict get $info wpws_ns] >= 0.0 &&
        [dict get $info route_status] eq "PASS" && [dict get $info bus_skew_status] eq "PASS"}]
    foreach key {tns_ns ths_ns tpws_ns setup_failing_endpoints hold_failing_endpoints \
                 pulse_width_failing_endpoints unconstrained_internal_endpoints \
                 route_errors missing_constraint_objects} {
        if {[dict get $info $key] != 0} {set qualifies 0}
    }
    dict set info status [expr {$qualifies ? "target_met" : "measured"}]
    dict set info utc_end [timing_run::utc_now]
    dict set info final_dcp_sha256 [timing_run::sha256_file [file join $out final.dcp]]
    foreach key {xdc_sha256 video_cdc_xdc_sha256 synthesis_dcp_sha256 final_dcp_sha256} {
        if {![regexp {^[0-9a-f]{64}$} [dict get $info $key]]} {
            error "Missing or invalid input/output hash: $key"
        }
    }
    timing_run::write_manifest $manifest $info
    puts "EXPERIMENT_RESULT status=[dict get $info status] setup=[dict get $info wns_ns] hold=[dict get $info whs_ns]"
    close_project
} on error {message options} {
    dict set info status failed
    dict set info utc_end [timing_run::utc_now]
    timing_run::write_manifest $manifest $info
    return -options $options $message
}
