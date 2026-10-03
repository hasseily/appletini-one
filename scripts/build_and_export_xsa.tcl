set xpr_path "project/appletini_yarz.xpr"
set xsa_out  "project/appletini_yarz_top.xsa"
# Full builds are the default; incremental reuse must name an explicit DCP.
set incremental_ref ""
if {[info exists ::env(APPLETINI_INCREMENTAL_REF_DCP)]} {
    set incremental_ref [file normalize $::env(APPLETINI_INCREMENTAL_REF_DCP)]
    if {![file isfile $incremental_ref]} {
        error "APPLETINI_INCREMENTAL_REF_DCP does not name a checkpoint."
    }
}
source [file join [file dirname [info script]] timing_run_helpers.tcl]
set force_full_build [expr {
    [info exists ::env(APPLETINI_FULL_BUILD)] &&
    $::env(APPLETINI_FULL_BUILD) ne "" &&
    $::env(APPLETINI_FULL_BUILD) ne "0"
}]
set timing_diagnostics [timing_run::env_enabled APPLETINI_TIMING_DIAGNOSTICS]
set minimum_setup_slack 0.050
# Explicit test-firmware policy: require strictly positive setup slack, with
# all hold, pulse-width, route and constraint checks unchanged. This does
# not change the separate known-good release promotion policy.
set positive_slack_only [timing_run::env_enabled APPLETINI_POSITIVE_SLACK_ONLY]
if {$positive_slack_only} {
    set minimum_setup_slack 0.000
} elseif {[info exists ::env(APPLETINI_MIN_SETUP_SLACK_NS)]} {
    set minimum_setup_slack $::env(APPLETINI_MIN_SETUP_SLACK_NS)
    if {![string is double -strict $minimum_setup_slack] ||
        !($minimum_setup_slack >= 0.050 && $minimum_setup_slack <= 1.0)} {
        error "APPLETINI_MIN_SETUP_SLACK_NS must be at least 0.050 and at most 1.0."
    }
}
set implementation_setup_margin 0.200
set script_dir [file dirname [file normalize [info script]]]
set margin_apply_hook \
    [file normalize [file join $script_dir apply_fabric_timing_margin.tcl]]
set margin_clear_hook \
    [file normalize [file join $script_dir clear_fabric_timing_margin.tcl]]
set route_finish_hook \
    [file normalize [file join $script_dir finish_video_timing.tcl]]
foreach hook [list $margin_apply_hook $margin_clear_hook $route_finish_hook] {
    if {![file isfile $hook]} {
        error "Timing-margin hook does not exist: $hook"
    }
}
set margin_apply_hook_sha256 [timing_run::sha256_file $margin_apply_hook]
set margin_clear_hook_sha256 [timing_run::sha256_file $margin_clear_hook]
set route_finish_hook_sha256 [timing_run::sha256_file $route_finish_hook]
foreach hook_hash [list $margin_apply_hook_sha256 $margin_clear_hook_sha256 $route_finish_hook_sha256] {
    if {![regexp -nocase {^[0-9a-f]{64}$} $hook_hash]} {
        error "Could not record a timing-margin hook SHA-256."
    }
}
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

dict set build_info vivado_version [version -short]
dict set build_info jobs 8
dict set build_info worker_threads ""
dict set build_info constraint_bounds_status ""
dict set build_info rescue_used 0
dict set build_info synthesis_reused 0
dict set build_info minimum_wns_ns $minimum_setup_slack
dict set build_info implementation_setup_margin_ns $implementation_setup_margin
dict set build_info margin_apply_hook_sha256 $margin_apply_hook_sha256
dict set build_info margin_clear_hook_sha256 $margin_clear_hook_sha256
dict set build_info route_finish_hook_sha256 $route_finish_hook_sha256
dict set build_info final_fabric_user_uncertainty_ns ""
dict set build_info final_pixel_user_uncertainty_ns ""
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
source [file join $script_dir register_video_cdc_constraints.tcl]

set device_part [timing_run::safe_property [current_project] PART ""]
dict set build_info device_part $device_part
if {[regexp {(-[0-9][A-Za-z]?)$} $device_part -> speed_grade]} {
    dict set build_info speed_grade $speed_grade
}

set synth_run [get_runs synth_1]
set impl_run [get_runs impl_1]

# Share one fixed profile with freshly created projects.
source [file join $script_dir configure_vivado_run_profile.tcl]
dict set build_info worker_threads [get_param general.maxThreads]

dict set build_info synth_strategy [timing_run::safe_property $synth_run STRATEGY ""]
dict set build_info synth_retiming \
    [timing_run::safe_property $synth_run STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING ""]
dict set build_info control_set_opt_threshold \
    [timing_run::safe_property $synth_run STEPS.SYNTH_DESIGN.ARGS.CONTROL_SET_OPT_THRESHOLD ""]
dict set build_info impl_strategy [timing_run::safe_property $impl_run STRATEGY ""]
dict set build_info opt_directive \
    [get_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE $impl_run]
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
    appletini_run_profile::set_if_changed [get_runs synth_1] AUTO_INCREMENTAL_CHECKPOINT 0
    appletini_run_profile::set_if_changed [get_runs synth_1] INCREMENTAL_CHECKPOINT ""
    if {[get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs synth_1]] ||
        [string trim [get_property INCREMENTAL_CHECKPOINT [get_runs synth_1]]] ne ""} {
        error "Synthesis incremental settings did not clear."
    }
}

# Reuse placement/routing only when an explicit reference was requested.
if {[llength [get_runs impl_1 -quiet]]} {
    appletini_run_profile::set_if_changed [get_runs impl_1] AUTO_INCREMENTAL_CHECKPOINT 0
    appletini_run_profile::set_if_changed [get_runs impl_1] INCREMENTAL_CHECKPOINT ""
    if {$force_full_build} {
        puts "APPLETINI_FULL_BUILD requested; running without an incremental reference."
    } elseif {[file isfile $incremental_ref]} {
        set incremental_ref_abs [file normalize $incremental_ref]
        appletini_run_profile::set_if_changed [get_runs impl_1] INCREMENTAL_CHECKPOINT $incremental_ref_abs
        dict set build_info incremental_reference $incremental_ref_abs
        dict set build_info incremental_reference_sha256 [timing_run::sha256_file $incremental_ref_abs]
        puts "Using incremental implementation reference: $incremental_ref_abs"
    } else {
        puts "No explicit incremental reference; running a full implementation."
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
if {[timing_run::env_enabled APPLETINI_REUSE_SYNTH]} {
    if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!" ||
        [get_property NEEDS_REFRESH [get_runs synth_1]]} {
        error "APPLETINI_REUSE_SYNTH requires a complete, current synthesis run."
    }
    dict set build_info synthesis_reused 1
    puts "Reusing the current synthesis run from the top-level compile check."
} else {
    set synthesis_started [clock seconds]
    reset_run synth_1 -quiet
    launch_runs synth_1 -jobs 8
    wait_on_run synth_1
    dict set build_info synthesis_wall_seconds [expr {[clock seconds] - $synthesis_started}]
}
if {[get_property STATUS $synth_run] ne "synth_design Complete!" ||
    [get_property NEEDS_REFRESH $synth_run]} {
    error "Synthesis did not finish with current inputs: [get_property STATUS $synth_run]"
}

puts "Launching implementation to write_bitstream..."
reset_run impl_1 -quiet
set implementation_started [clock seconds]
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
dict set build_info implementation_wall_seconds [expr {[clock seconds] - $implementation_started}]
set implementation_status [get_property STATUS $impl_run]
if {$implementation_status ni {
        "write_bitstream Complete!" "write_bitstream Complete, Failed Timing!"
    } || [get_property NEEDS_REFRESH $impl_run]} {
    error "Implementation did not finish write_bitstream with current inputs: $implementation_status"
}
set impl_dir [get_property DIRECTORY $impl_run]
set bitstream_path [file join $impl_dir appletini_yarz_top.bit]
if {![file isfile $bitstream_path] || [file size $bitstream_path] == 0 ||
    [file mtime $bitstream_path] < $implementation_started} {
    error "Implementation did not produce a fresh bitstream."
}

# Save a timing-clean final design as a candidate. Timing closure alone does
# not prove that the image boots on hardware, so only the explicit promotion
# script may replace the known-good incremental reference.
open_run impl_1
foreach {domain clock_name} {fabric clk_out1_zynq_ps_bd_clk_wiz_1_0
                            pixel clk_out1_zynq_ps_bd_clk_wiz_0_0} {
    set domain_clock [get_clocks -quiet $clock_name]
    if {[llength $domain_clock] != 1} {error "Expected one $domain clock at signoff."}
    set domain_path [get_timing_paths -quiet -delay_type max \
        -from $domain_clock -to $domain_clock -max_paths 1]
    if {[llength $domain_path] != 1} {error "No $domain setup path at signoff."}
    set final_user_uncertainty [get_property USER_UNCERTAINTY $domain_path]
    if {$final_user_uncertainty eq ""} {set final_user_uncertainty 0.000}
    dict set build_info final_${domain}_user_uncertainty_ns $final_user_uncertainty
    if {![string is double -strict $final_user_uncertainty] ||
        abs(double($final_user_uncertainty)) > 0.0005} {
        error "Temporary $domain setup margin remains at signoff: $final_user_uncertainty ns."
    }
}
set fabric_clock [get_clocks clk_out1_zynq_ps_bd_clk_wiz_1_0]
# Check the reopened design independently of the implementation hook process.
# Final setup slack must use the board's original output timing requirements.
foreach spec {
    {a2fpga_dir_a fabric 10.000 final_direction_a_limit_ns}
    {a2fpga_dir_d fabric 10.000 final_direction_d_limit_ns}
    {a2fpga_dir_d phi0 8.000 final_phi0_release_limit_ns}
} {
    lassign $spec port source expected manifest_key
    set output_port [get_ports -quiet $port]
    set startpoint [expr {$source eq "fabric" ? $fabric_clock :
        [get_ports -quiet a2fpga_clk]}]
    set output_path [get_timing_paths -quiet -delay_type max \
        -from $startpoint -to $output_port -max_paths 1]
    if {[llength $output_port] != 1 || [llength $startpoint] != 1 ||
        [llength $output_path] != 1} {
        error "Missing $source to $port path at signoff."
    }
    set output_limit [get_property REQUIREMENT $output_path]
    if {![string is double -strict $output_limit] ||
        abs(double($output_limit) - $expected) > 0.0005} {
        error "Expected nominal $expected ns from $source to $port, got $output_limit ns."
    }
    dict set build_info $manifest_key $output_limit
}
set worst_setup_path [get_timing_paths -quiet -delay_type max -max_paths 1]
set worst_hold_path  [get_timing_paths -quiet -delay_type min -max_paths 1]
if {[llength $worst_setup_path] == 0 || [llength $worst_hold_path] == 0} {
    error "Unable to verify setup/hold timing before checkpoint promotion."
}
set worst_setup_slack [get_property SLACK $worst_setup_path]
set worst_hold_slack  [get_property SLACK $worst_hold_path]
puts "Final implementation slack: setup=$worst_setup_slack ns hold=$worst_hold_slack ns"

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
    [file join [get_property DIRECTORY [get_runs synth_1]] runme.log] \
    [file join [get_property DIRECTORY [get_runs impl_1]] runme.log]] {
    if {![file isfile $log_path]} {
        error "Missing current run log: $log_path"
    }
    incr missing_constraint_objects \
        [timing_run::count_missing_constraint_objects [timing_run::read_text $log_path]]
}
dict set build_info missing_constraint_objects $missing_constraint_objects

set rank 0
foreach path [get_timing_paths -delay_type max -max_paths 10 -sort_by slack] {
    incr rank
    timing_run::append_path [timing_run::path_values $build_id $rank $path]
}

# Verify that higher-priority exceptions did not mask the Apple/Gray bounds.
source [file join $script_dir check_video_bus_constraints.tcl]
dict set build_info constraint_bounds_status PASS

# Keep reports and CSV values for failed attempts, but never export hardware
# from a design with a timing, route, bus-skew, or constraint fault.
if {![string is double -strict [dict get $build_info wns_ns]] ||
    [dict get $build_info wns_ns] < $minimum_setup_slack ||
    ($positive_slack_only && [dict get $build_info wns_ns] <= 0.0)} {
    error "Timing failed (wns_ns below $minimum_setup_slack ns); refusing to export hardware."
}
foreach key {whs_ns wpws_ns} {
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
