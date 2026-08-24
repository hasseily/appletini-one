if {[llength $argv] != 2} {
    error "Usage: export_positive_slack_test_candidate.tcl <checkpoint> <timing-run-dir>"
}

set checkpoint_path [file normalize [lindex $argv 0]]
set timing_run_dir [file normalize [lindex $argv 1]]

if {![file isfile $checkpoint_path]} {
    error "Checkpoint does not exist: $checkpoint_path"
}

# Keep Vivado batch mode on the built-in Tcl Store. User Tcl apps can fail
# while a checkpoint loads and must not affect this guarded export.
set tcl_store_candidates {}
if {[info exists ::env(XILINX_VIVADO)]} {
    lappend tcl_store_candidates \
        [file join $::env(XILINX_VIVADO) data XilinxTclStore]
}
if {![catch {version -short} vivado_version]} {
    lappend tcl_store_candidates \
        [file join C:/ Xilinx $vivado_version Vivado data XilinxTclStore]
}
foreach tcl_store_candidate $tcl_store_candidates {
    set tcl_store_candidate [file normalize $tcl_store_candidate]
    if {[file isdirectory $tcl_store_candidate]} {
        set ::env(XILINX_TCLAPP_REPO) $tcl_store_candidate
        set ::env(XILINX_LOCAL_USER_DATA) "NO"
        puts "Using Vivado Tcl Store: $tcl_store_candidate"
        break
    }
}
unset tcl_store_candidate
unset tcl_store_candidates

source [file join [file dirname [info script]] timing_run_helpers.tcl]

file mkdir $timing_run_dir
set parent_manifest_path [file join $timing_run_dir manifest.txt]
set candidate_manifest_path \
    [file join $timing_run_dir positive_slack_test_manifest.txt]
set parent_manifest [timing_run::read_manifest $parent_manifest_path]

set candidate_info [dict create \
    status started \
    parent_build_id [dict get $parent_manifest build_id] \
    git_sha [dict get $parent_manifest git_sha] \
    git_dirty [dict get $parent_manifest git_dirty] \
    input_checkpoint $checkpoint_path \
    input_checkpoint_sha256 [timing_run::sha256_file $checkpoint_path] \
    wns_ns "" \
    tns_ns "" \
    whs_ns "" \
    ths_ns "" \
    wpws_ns "" \
    tpws_ns "" \
    setup_failing_endpoints "" \
    hold_failing_endpoints "" \
    pulse_width_failing_endpoints "" \
    unconstrained_internal_endpoints "" \
    route_status "" \
    route_errors "" \
    bus_skew_status "" \
    bus_skew_wns_ns "" \
    missing_constraint_objects [dict get $parent_manifest missing_constraint_objects] \
    candidate_dcp_sha256 "" \
    bitstream_sha256 "" \
    xsa_sha256 ""]
timing_run::write_manifest $candidate_manifest_path $candidate_info

puts "Opening the full build's final checkpoint: $checkpoint_path"
open_checkpoint $checkpoint_path

set timing_summary_path \
    [file join $timing_run_dir positive_slack_test_timing_summary.rpt]
set route_status_path \
    [file join $timing_run_dir positive_slack_test_route_status.rpt]
set bus_skew_path \
    [file join $timing_run_dir positive_slack_test_bus_skew.rpt]
set check_timing_path \
    [file join $timing_run_dir positive_slack_test_check_timing.rpt]
set methodology_path \
    [file join $timing_run_dir positive_slack_test_methodology.rpt]
set candidate_dcp_path \
    [file join $timing_run_dir positive_slack_test_candidate.dcp]
set bitstream_path [file join $timing_run_dir \
    appletini_yarz_top_F0.9.99_dual_ssi263_positive_slack_test.bit]
set xsa_path [file join $timing_run_dir \
    appletini_yarz_top_F0.9.99_dual_ssi263_positive_slack_test.xsa]

report_timing_summary -max_paths 10 -report_unconstrained \
    -warn_on_violation -file $timing_summary_path
report_route_status -file $route_status_path
report_bus_skew -warn_on_violation -file $bus_skew_path
check_timing -verbose -file $check_timing_path
report_methodology -file $methodology_path
write_checkpoint -force $candidate_dcp_path

set candidate_info [dict merge $candidate_info \
    [timing_run::parse_timing_summary \
        [timing_run::read_text $timing_summary_path]]]
set candidate_info [dict merge $candidate_info \
    [timing_run::parse_route_status \
        [timing_run::read_text $route_status_path]]]
set candidate_info [dict merge $candidate_info \
    [timing_run::parse_bus_skew \
        [timing_run::read_text $bus_skew_path]]]
dict set candidate_info candidate_dcp_sha256 \
    [timing_run::sha256_file $candidate_dcp_path]
dict set candidate_info status analyzed
timing_run::write_manifest $candidate_manifest_path $candidate_info

foreach key {wns_ns whs_ns wpws_ns} {
    if {![string is double -strict [dict get $candidate_info $key]] ||
        [dict get $candidate_info $key] < 0.0} {
        dict set candidate_info status rejected
        timing_run::write_manifest $candidate_manifest_path $candidate_info
        error "Timing failed ($key); refusing to export test hardware."
    }
}
foreach key {
    tns_ns ths_ns tpws_ns setup_failing_endpoints
    hold_failing_endpoints pulse_width_failing_endpoints
    unconstrained_internal_endpoints route_errors
    missing_constraint_objects
} {
    if {![string is double -strict [dict get $candidate_info $key]] ||
        [dict get $candidate_info $key] != 0.0} {
        dict set candidate_info status rejected
        timing_run::write_manifest $candidate_manifest_path $candidate_info
        error "Candidate check failed ($key); refusing to export test hardware."
    }
}
if {[dict get $candidate_info route_status] ne "PASS" ||
    [dict get $candidate_info bus_skew_status] ne "PASS"} {
    dict set candidate_info status rejected
    timing_run::write_manifest $candidate_manifest_path $candidate_info
    error "Route or bus-skew checks failed; refusing to export test hardware."
}

puts "Writing the positive-slack test bitstream: $bitstream_path"
write_bitstream -force $bitstream_path
puts "Writing the matching test hardware platform: $xsa_path"
write_hw_platform -fixed -include_bit -force -file $xsa_path

dict set candidate_info bitstream_sha256 \
    [timing_run::sha256_file $bitstream_path]
dict set candidate_info xsa_sha256 [timing_run::sha256_file $xsa_path]
dict set candidate_info status positive_slack_test_exported
timing_run::write_manifest $candidate_manifest_path $candidate_info

puts "Test candidate setup margin: [dict get $candidate_info wns_ns] ns"
puts "Test candidate hold margin: [dict get $candidate_info whs_ns] ns"
puts "Positive-slack test candidate exported without further implementation."
close_design
