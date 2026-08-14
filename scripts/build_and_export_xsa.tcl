set xpr_path "project/appletini_yarz.xpr"
set xsa_out  "project/appletini_yarz_top.xsa"
set incremental_ref       ".vivado_cache/appletini_yarz_top_known_good.dcp"
source [file join [file dirname [info script]] timing_run_helpers.tcl]
set force_full_build [expr {
    [info exists ::env(APPLETINI_FULL_BUILD)] &&
    $::env(APPLETINI_FULL_BUILD) ne "" &&
    $::env(APPLETINI_FULL_BUILD) ne "0"
}]
set timing_diagnostics [timing_run::env_enabled APPLETINI_TIMING_DIAGNOSTICS]
set build_mode [expr {
    $force_full_build || ![file isfile $incremental_ref]
        ? "full"
        : "incremental"
}]
set build_info [timing_run::new_build $build_mode]
set build_id [dict get $build_info build_id]
set timing_run_dir [dict get $build_info run_dir]
set build_manifest [file join $timing_run_dir manifest.txt]
set build_failed 1

proc finish_timing_build {} {
    global build_failed build_info build_manifest
    if {!$build_failed} {
        return
    }
    dict set build_info utc_end [timing_run::utc_now]
    dict set build_info status failed
    timing_run::write_manifest $build_manifest $build_info
    timing_run::append_build $build_info
}

dict set build_info vivado_version [version]
dict set build_info jobs 8
dict set build_info rescue_used 0
dict set build_info seed_control "Vivado default"
dict set build_info device_part ""
dict set build_info speed_grade ""
dict set build_info incremental_reference ""
dict set build_info incremental_reference_sha256 ""
foreach key {
    wns_ns tns_ns whs_ns ths_ns wpws_ns tpws_ns
    setup_failing_endpoints hold_failing_endpoints
    pulse_width_failing_endpoints
    unconstrained_internal_endpoints route_status route_errors
    bus_skew_status bus_skew_wns_ns
    slices luts lut_logic lut_memory shift_register_luts registers
    f7_muxes f8_muxes carry4s bram_tiles ramb36 ramb18 dsps control_sets
    missing_constraint_objects candidate_dcp_sha256
    bitstream_sha256 xsa_sha256
} {
    dict set build_info $key ""
}
timing_run::write_manifest $build_manifest $build_info
puts "Timing build ID: $build_id"
puts "Timing artifacts: [file normalize $timing_run_dir]"

try {

# Child OOC/IP synthesis runs inherit the environment from this batch process.
# Keep them on Vivado's built-in Tcl Store so user-installed Tcl apps cannot
# break generated run scripts.
proc configure_batch_tclapp_repo {} {
    set candidates {}

    if {[info exists ::env(XILINX_VIVADO)]} {
        lappend candidates [file join $::env(XILINX_VIVADO) data XilinxTclStore]
    }

    if {![catch {version -short} vivado_version]} {
        lappend candidates [file join C:/ Xilinx $vivado_version Vivado data XilinxTclStore]
    }

    foreach candidate $candidates {
        set repo [file normalize $candidate]
        if {[file isdirectory $repo]} {
            set ::env(XILINX_TCLAPP_REPO) $repo
            set ::env(XILINX_LOCAL_USER_DATA) "NO"
            puts "Using Vivado Tcl Store for batch runs: $repo"
            return
        }
    }

    puts "WARNING: Vivado Tcl Store not found; batch runs may load user Tcl apps."
}

configure_batch_tclapp_repo

puts "Opening project: $xpr_path"
open_project $xpr_path

set device_part [timing_run::safe_property [current_project] PART ""]
dict set build_info device_part $device_part
if {[regexp {(-[0-9][A-Za-z]?)$} $device_part -> speed_grade]} {
    dict set build_info speed_grade $speed_grade
}

set synth_run [get_runs synth_1]
set impl_run [get_runs impl_1]

# Keep all signoff flow settings in tracked Tcl. A generated project may hold
# stale run options, so apply and check the required placement, route, and
# post-route settings before the manifest records them.
set place_directive Explore
if {[timing_run::env_enabled APPLETINI_PLACE_DIRECTIVE]} {
    set place_directive [string trim $::env(APPLETINI_PLACE_DIRECTIVE)]
}
set allowed_place_directives {
    Explore ExtraNetDelay_high ExtraPostPlacementOpt AltSpreadLogic_high
}
if {[lsearch -exact $allowed_place_directives $place_directive] < 0} {
    error "Unsupported APPLETINI_PLACE_DIRECTIVE: $place_directive"
}
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE $place_directive $impl_run
set_property -dict \
    [list {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} {-tns_cleanup}] $impl_run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true $impl_run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl_run
if {[get_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE $impl_run] ne
        $place_directive} {
    error "Place directive did not apply."
}
if {[string trim [get_property {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} $impl_run]] ne
        "-tns_cleanup"} {
    error "Route extra options did not apply."
}
if {![get_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED $impl_run] ||
    [get_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE $impl_run] ne
        "Explore"} {
    error "Post-route physical optimization settings did not apply."
}
puts "Placement directive: $place_directive"

dict set build_info synth_strategy [timing_run::safe_property $synth_run STRATEGY ""]
dict set build_info synth_retiming \
    [timing_run::safe_property $synth_run STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING ""]
dict set build_info control_set_opt_threshold \
    [timing_run::safe_property $synth_run STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD ""]
dict set build_info impl_strategy [timing_run::safe_property $impl_run STRATEGY ""]
dict set build_info place_directive \
    [timing_run::safe_property $impl_run STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ""]
dict set build_info phys_opt_directive \
    [timing_run::safe_property $impl_run STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE ""]
set route_directive \
    [timing_run::safe_property $impl_run STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE ""]
set route_more_options \
    [timing_run::safe_property $impl_run {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} ""]
dict set build_info route_directive \
    "directive=$route_directive;more_options=$route_more_options"
set post_route_enabled \
    [timing_run::safe_property $impl_run STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED ""]
set post_route_directive \
    [timing_run::safe_property $impl_run STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE ""]
dict set build_info post_route_phys_opt_directive \
    "enabled=$post_route_enabled;directive=$post_route_directive"

# Always synthesize from current RTL. Generated partitions in an incremental
# checkpoint may not match the source being validated.
if {[llength [get_runs synth_1 -quiet]]} {
    set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs synth_1]
    set_property INCREMENTAL_CHECKPOINT "" [get_runs synth_1]
    if {[get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs synth_1]] ||
        [string trim [get_property INCREMENTAL_CHECKPOINT [get_runs synth_1]]] ne ""} {
        error "Synthesis incremental settings did not clear."
    }
}

# Reuse placement/routing from the last timing-clean implementation. Vivado
# matches unchanged cells and implements unmatched logic normally, so source
# changes can still proceed without treating the reference as a fixed netlist.
if {[llength [get_runs impl_1 -quiet]]} {
    set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs impl_1]
    set_property INCREMENTAL_CHECKPOINT "" [get_runs impl_1]
    if {$force_full_build} {
        puts "APPLETINI_FULL_BUILD requested; running without an incremental reference."
    } elseif {[file isfile $incremental_ref]} {
        set incremental_ref_abs [file normalize $incremental_ref]
        set_property INCREMENTAL_CHECKPOINT $incremental_ref_abs [get_runs impl_1]
        dict set build_info incremental_reference $incremental_ref_abs
        dict set build_info incremental_reference_sha256 [timing_run::sha256_file $incremental_ref_abs]
        puts "Using incremental implementation reference: $incremental_ref_abs"
    } else {
        puts "No incremental implementation reference found; running a full implementation."
    }
    if {[get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs impl_1]]} {
        error "Implementation auto-incremental mode did not clear."
    }
    set active_incremental_ref \
        [string trim [get_property INCREMENTAL_CHECKPOINT [get_runs impl_1]]]
    if {$build_mode eq "full" && $active_incremental_ref ne ""} {
        error "Full build still has an incremental checkpoint: $active_incremental_ref"
    }
    if {$build_mode eq "incremental" && $active_incremental_ref eq ""} {
        error "Incremental build has no active reference checkpoint."
    }
}

# If you need a specific top, you can uncomment and set it explicitly:
# set_property top appletini_yarz_top [current_fileset]

# check for syntax errors
puts "Checking syntax..."
check_syntax

# Run synthesis + implementation through write_bitstream
puts "Launching synthesis..."
reset_run synth_1 -quiet
launch_runs synth_1 -jobs 8
wait_on_run synth_1

puts "Launching implementation to write_bitstream..."
reset_run impl_1 -quiet
# Post-route physical optimization. This design sits at the edge of the
# 133 MHz fabric-clock timing budget, so a from-scratch (non-incremental)
# placement can leave sub-100 ps setup violations on pre-existing
# carry-chain paths (the audio mixer, etc.). Post-route phys_opt closes
# these deterministically; it is a no-op once timing is already met.
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

# Save a timing-clean final design as a candidate. Timing closure alone does
# not prove that the image boots on hardware, so only the explicit promotion
# script may replace the known-good incremental reference.
open_run impl_1
set worst_setup_path [get_timing_paths -quiet -delay_type max -max_paths 1]
set worst_hold_path  [get_timing_paths -quiet -delay_type min -max_paths 1]
if {[llength $worst_setup_path] == 0 || [llength $worst_hold_path] == 0} {
    error "Unable to verify setup/hold timing before checkpoint promotion."
}
set worst_setup_slack [get_property SLACK $worst_setup_path]
set worst_hold_slack  [get_property SLACK $worst_hold_path]
puts "Final implementation slack: setup=$worst_setup_slack ns hold=$worst_hold_slack ns"

# A full placement can finish a few picoseconds short even after the run's
# normal post-route Explore pass. Do not change the placement strategy or
# accept that result. One AggressiveExplore pass on the routed design can
# transform only the remaining critical cones while preserving the completed
# placement and routing. This closed the F0.9.75 image from -0.025 ns to
# +0.022 ns. Keep hold failures fatal; this fallback targets setup only.
if {$worst_setup_slack < 0.0 && $worst_hold_slack >= 0.0} {
    puts "Setup timing is short; running one extra post-route AggressiveExplore pass."
    dict set build_info rescue_used 1
    phys_opt_design -directive AggressiveExplore

    set worst_setup_path [get_timing_paths -quiet -delay_type max -max_paths 1]
    set worst_hold_path  [get_timing_paths -quiet -delay_type min -max_paths 1]
    set worst_setup_slack [get_property SLACK $worst_setup_path]
    set worst_hold_slack  [get_property SLACK $worst_hold_path]
    puts "Slack after extra physical optimization: setup=$worst_setup_slack ns hold=$worst_hold_slack ns"

    if {$worst_setup_slack >= 0.0 && $worst_hold_slack >= 0.0} {
        set impl_dir [get_property DIRECTORY [get_runs impl_1]]
        write_checkpoint -force [file join $impl_dir appletini_yarz_top_postroute_physopt.dcp]
        report_timing_summary -file [file join $impl_dir appletini_yarz_top_timing_summary_postroute_physopted.rpt]
        report_bus_skew -warn_on_violation -file [file join $impl_dir appletini_yarz_top_bus_skew_postroute_physopted.rpt]
        write_bitstream -force [file join $impl_dir appletini_yarz_top.bit]
    }
}

# Save signoff reports next to the immutable build record. The main run can
# overwrite its reports on the next build; this directory never does.
set timing_summary_path [file join $timing_run_dir timing_summary.rpt]
set utilization_path [file join $timing_run_dir utilization.rpt]
set control_sets_path [file join $timing_run_dir control_sets.rpt]
set route_status_path [file join $timing_run_dir route_status.rpt]
set bus_skew_path [file join $timing_run_dir bus_skew.rpt]
set check_timing_path [file join $timing_run_dir check_timing.rpt]
set methodology_path [file join $timing_run_dir methodology.rpt]
set clock_interaction_path [file join $timing_run_dir clock_interaction.rpt]

report_methodology -file $methodology_path
check_timing -verbose -file $check_timing_path
report_clock_interaction -file $clock_interaction_path
report_timing_summary -max_paths 10 -report_unconstrained \
    -warn_on_violation -file $timing_summary_path
report_utilization -file $utilization_path
report_control_sets -verbose -file $control_sets_path
report_route_status -file $route_status_path
report_bus_skew -warn_on_violation -file $bus_skew_path

if {$timing_diagnostics} {
    puts "Writing extended timing diagnostics."
    report_design_analysis -congestion \
        -file [file join $timing_run_dir design_analysis_congestion.rpt]
    report_high_fanout_nets -timing -load_types -max_nets 100 \
        -file [file join $timing_run_dir high_fanout_nets.rpt]
    report_qor_suggestions -file [file join $timing_run_dir qor_suggestions.rpt]
}

dict set build_info utc_end [timing_run::utc_now]
dict set build_info status analyzed
set timing_text [timing_run::read_text $timing_summary_path]
set utilization_text [timing_run::read_text $utilization_path]
set route_text [timing_run::read_text $route_status_path]
set bus_skew_text [timing_run::read_text $bus_skew_path]
set build_info [dict merge $build_info [timing_run::parse_timing_summary $timing_text]]
set build_info [dict merge $build_info [timing_run::parse_utilization $utilization_text]]
set build_info [dict merge $build_info [timing_run::parse_route_status $route_text]]
set build_info [dict merge $build_info [timing_run::parse_bus_skew $bus_skew_text]]
set missing_constraint_objects 0
foreach log_path [list \
    vivado.log \
    [file join [get_property DIRECTORY [get_runs synth_1]] runme.log] \
    [file join [get_property DIRECTORY [get_runs impl_1]] runme.log]] {
    if {[file isfile $log_path]} {
        incr missing_constraint_objects \
            [timing_run::count_missing_constraint_objects [timing_run::read_text $log_path]]
    }
}
dict set build_info missing_constraint_objects $missing_constraint_objects

set rank 0
foreach path [get_timing_paths -delay_type max -max_paths 10 -sort_by slack] {
    incr rank
    timing_run::append_path [timing_run::path_values $build_id $rank $path]
}

# Keep reports and CSV values for failed attempts, but never export hardware
# from a design with a timing, route, bus-skew, or constraint fault.
foreach key {wns_ns whs_ns wpws_ns} {
    if {![string is double -strict [dict get $build_info $key]] ||
        [dict get $build_info $key] < 0.0} {
        error "Timing failed ($key); refusing to export hardware."
    }
}
foreach key {
    tns_ns ths_ns tpws_ns setup_failing_endpoints
    hold_failing_endpoints pulse_width_failing_endpoints
    unconstrained_internal_endpoints route_errors
    missing_constraint_objects
} {
    if {![string is double -strict [dict get $build_info $key]] ||
        [dict get $build_info $key] != 0.0} {
        error "Build check failed ($key); refusing to export hardware."
    }
}
if {[dict get $build_info route_status] ne "PASS" ||
    [dict get $build_info bus_skew_status] ne "PASS"} {
    error "Route or bus-skew checks failed; refusing to export hardware."
}

set build_candidate [file join $timing_run_dir candidate.dcp]
write_checkpoint -force $build_candidate
dict set build_info candidate_dcp_sha256 [timing_run::sha256_file $build_candidate]

puts "Saved timing-clean build candidate: [file normalize $build_candidate]"

# Export XSA including bitstream
# write_hw_platform is the modern flow; include_bit ensures bit is packaged.
set impl_dir [get_property DIRECTORY [get_runs impl_1]]
set bitstream_path [file join $impl_dir appletini_yarz_top.bit]
set build_bitstream [file join $timing_run_dir appletini_yarz_top.bit]
set build_xsa [file join $timing_run_dir appletini_yarz_top.xsa]
file copy -force $bitstream_path $build_bitstream
puts "Exporting immutable hardware platform: [file normalize $build_xsa]"
write_hw_platform -fixed -include_bit -force -file $build_xsa
file copy -force $build_xsa $xsa_out
dict set build_info bitstream_sha256 [timing_run::sha256_file $build_bitstream]
dict set build_info xsa_sha256 [timing_run::sha256_file $build_xsa]
dict set build_info status exported
dict set build_info utc_end [timing_run::utc_now]
timing_run::write_manifest $build_manifest $build_info
timing_run::append_build $build_info
set build_failed 0

puts "Timing build recorded: $build_id"
puts "After a second qualifying full build and hardware validation, promote both explicit build IDs."

close_project
puts "Done."
} on error {message options} {
    catch {finish_timing_build}
    return -options $options $message
}
