# Improve positive but short 133 MHz setup paths without rebuilding or
# changing the video clock groups.
#
# Usage:
#   vivado -mode batch -source scripts/refine_fabric_timing.tcl -tclargs \
#       INPUT_DCP OUTPUT_DIR ?TEMPORARY_UNCERTAINTY_NS? ?MINIMUM_WNS_NS?

if {$argc < 2 || $argc > 4} {
    error "Usage: refine_fabric_timing.tcl INPUT_DCP OUTPUT_DIR ?TEMPORARY_UNCERTAINTY_NS? ?MINIMUM_WNS_NS?"
}

set input_dcp [file normalize [lindex $argv 0]]
set output_dir [file normalize [lindex $argv 1]]
set temporary_uncertainty [expr {$argc >= 3 ? [lindex $argv 2] : 0.200}]
set minimum_wns [expr {$argc >= 4 ? [lindex $argv 3] : 0.150}]

if {![file isfile $input_dcp]} {
    error "Input checkpoint does not exist: $input_dcp"
}
foreach value [list $temporary_uncertainty $minimum_wns] {
    if {![string is double -strict $value] || $value <= 0.0} {
        error "Timing margins must be positive numbers."
    }
}
if {$temporary_uncertainty <= $minimum_wns} {
    error "Temporary uncertainty must exceed the required final WNS."
}

set output_dcp [file join $output_dir candidate.dcp]
set output_bit [file join $output_dir appletini_yarz_top.bit]
set output_manifest [file join $output_dir manifest.txt]
foreach path [list $output_dcp $output_bit $output_manifest] {
    if {[file exists $path]} {
        error "Refusing to overwrite timing artifact: $path"
    }
}
file mkdir $output_dir

source [file join [file dirname [info script]] timing_run_helpers.tcl]

proc clock_setup_slack {clock_name} {
    set clock [get_clocks -quiet $clock_name]
    if {[llength $clock] != 1} {
        error "Expected one clock named $clock_name."
    }
    set path [get_timing_paths -quiet -delay_type max -from $clock -to $clock \
        -max_paths 1]
    if {[llength $path] != 1} {
        error "No setup path found for clock $clock_name."
    }
    return [get_property SLACK $path]
}

proc cross_clock_setup_slack {from_name to_name} {
    set from_clock [get_clocks -quiet $from_name]
    set to_clock [get_clocks -quiet $to_name]
    if {[llength $from_clock] != 1 || [llength $to_clock] != 1} {
        error "Expected one clock for $from_name and $to_name."
    }
    set path [get_timing_paths -quiet -delay_type max \
        -from $from_clock -to $to_clock -max_paths 1]
    if {[llength $path] != 1} {
        error "No setup path found from $from_name to $to_name."
    }
    return [get_property SLACK $path]
}

proc global_slack {delay_type} {
    set path [get_timing_paths -quiet -delay_type $delay_type -max_paths 1]
    if {[llength $path] != 1} {
        error "No $delay_type timing path found."
    }
    return [get_property SLACK $path]
}

set fabric_group clk_out1_zynq_ps_bd_clk_wiz_1_0
set video_group clk_out1_zynq_ps_bd_clk_wiz_0_0
set dvi_group dvi_clk_out

open_checkpoint $input_dcp

set fabric_before [clock_setup_slack $fabric_group]
set video_before [clock_setup_slack $video_group]
set video_dvi_before [cross_clock_setup_slack $video_group $dvi_group]
set global_before [global_slack max]

puts "Fabric setup before refinement: $fabric_before ns"
puts "Global setup before refinement: $global_before ns"
puts "Video setup before refinement: $video_before ns"
puts "Video-to-DVI setup before refinement: $video_dvi_before ns"

set fabric_clock [get_clocks $fabric_group]
set_clock_uncertainty -setup $temporary_uncertainty $fabric_clock
puts "Temporary fabric setup uncertainty: $temporary_uncertainty ns"

# Explicit operations allow a path-group restriction. A directive would also
# optimize unrelated positive paths and could move the Apple video path.
phys_opt_design -placement_opt -routing_opt -restruct_opt \
    -critical_cell_opt -critical_pin_opt -path_groups $fabric_group

# The extra uncertainty is an implementation aid, not a design constraint.
set_clock_uncertainty -setup 0.0 $fabric_clock

set fabric_after [clock_setup_slack $fabric_group]
set video_after [clock_setup_slack $video_group]
set video_dvi_after [cross_clock_setup_slack $video_group $dvi_group]
set global_after [global_slack max]
set hold_after [global_slack min]

puts "Fabric setup after refinement: $fabric_after ns"
puts "Global setup after refinement: $global_after ns"
puts "Global hold after refinement: $hold_after ns"
puts "Video setup after refinement: $video_after ns"
puts "Video-to-DVI setup after refinement: $video_dvi_after ns"

if {$fabric_after <= $minimum_wns || $global_after <= $minimum_wns} {
    error "Refined setup slack must be greater than $minimum_wns ns."
}
if {$hold_after < 0.0} {
    error "Refinement introduced a hold violation."
}
if {$video_after + 0.001 < $video_before ||
    $video_dvi_after + 0.001 < $video_dvi_before} {
    error "Refinement degraded a protected video timing group."
}

set timing_report [file join $output_dir timing_summary.rpt]
set route_report [file join $output_dir route_status.rpt]
set bus_skew_report [file join $output_dir bus_skew.rpt]
set check_timing_report [file join $output_dir check_timing.rpt]
set methodology_report [file join $output_dir methodology.rpt]
set clock_report [file join $output_dir clock_interaction.rpt]
set utilization_report [file join $output_dir utilization.rpt]
set control_sets_report [file join $output_dir control_sets.rpt]

report_timing_summary -max_paths 20 -report_unconstrained \
    -warn_on_violation -file $timing_report
report_route_status -file $route_report
report_bus_skew -warn_on_violation -file $bus_skew_report
check_timing -verbose -file $check_timing_report
report_methodology -file $methodology_report
report_clock_interaction -file $clock_report
report_utilization -file $utilization_report
report_control_sets -verbose -file $control_sets_report

set timing_values [timing_run::parse_timing_summary \
    [timing_run::read_text $timing_report]]
set route_values [timing_run::parse_route_status \
    [timing_run::read_text $route_report]]
set bus_values [timing_run::parse_bus_skew \
    [timing_run::read_text $bus_skew_report]]

foreach key {wns_ns whs_ns wpws_ns} {
    if {![string is double -strict [dict get $timing_values $key]] ||
        [dict get $timing_values $key] < 0.0} {
        error "Final timing check failed: $key"
    }
}
if {[dict get $timing_values wns_ns] <= $minimum_wns} {
    error "Reported WNS does not exceed $minimum_wns ns."
}
foreach key {
    tns_ns ths_ns tpws_ns setup_failing_endpoints hold_failing_endpoints
    pulse_width_failing_endpoints unconstrained_internal_endpoints
} {
    if {![string is double -strict [dict get $timing_values $key]] ||
        [dict get $timing_values $key] != 0.0} {
        error "Final timing check failed: $key"
    }
}
if {[dict get $route_values route_status] ne "PASS" ||
    [dict get $route_values route_errors] != 0 ||
    [dict get $bus_values bus_skew_status] ne "PASS"} {
    error "Route or bus-skew check failed."
}

write_checkpoint -force $output_dcp
write_bitstream -force $output_bit

set source_manifest_path [file join [file dirname $input_dcp] manifest.txt]
set source_build_id ""
set source_git_sha ""
set source_missing_constraints ""
if {[file isfile $source_manifest_path]} {
    set source_values [timing_run::read_manifest $source_manifest_path]
    foreach {source_key output_var} {
        build_id source_build_id
        git_sha source_git_sha
        missing_constraint_objects source_missing_constraints
    } {
        if {[dict exists $source_values $source_key]} {
            set $output_var [dict get $source_values $source_key]
        }
    }
}

set manifest [dict create \
    status exported \
    build_mode postroute_fabric_margin \
    utc_end [timing_run::utc_now] \
    source_build_id $source_build_id \
    source_git_sha $source_git_sha \
    source_missing_constraint_objects $source_missing_constraints \
    input_dcp_sha256 [timing_run::sha256_file $input_dcp] \
    temporary_setup_uncertainty_ns $temporary_uncertainty \
    temporary_setup_uncertainty_cleared 1 \
    minimum_wns_ns $minimum_wns \
    fabric_wns_before_ns $fabric_before \
    fabric_wns_after_ns $fabric_after \
    global_wns_before_ns $global_before \
    global_wns_after_ns $global_after \
    global_whs_after_ns $hold_after \
    video_wns_before_ns $video_before \
    video_wns_after_ns $video_after \
    video_dvi_wns_before_ns $video_dvi_before \
    video_dvi_wns_after_ns $video_dvi_after \
    optimizer "phys_opt_design path-group-only" \
    wns_ns [dict get $timing_values wns_ns] \
    tns_ns [dict get $timing_values tns_ns] \
    whs_ns [dict get $timing_values whs_ns] \
    ths_ns [dict get $timing_values ths_ns] \
    wpws_ns [dict get $timing_values wpws_ns] \
    tpws_ns [dict get $timing_values tpws_ns] \
    setup_failing_endpoints [dict get $timing_values setup_failing_endpoints] \
    hold_failing_endpoints [dict get $timing_values hold_failing_endpoints] \
    pulse_width_failing_endpoints [dict get $timing_values pulse_width_failing_endpoints] \
    unconstrained_internal_endpoints [dict get $timing_values unconstrained_internal_endpoints] \
    route_status [dict get $route_values route_status] \
    route_errors [dict get $route_values route_errors] \
    bus_skew_status [dict get $bus_values bus_skew_status] \
    bus_skew_wns_ns [dict get $bus_values bus_skew_wns_ns] \
    candidate_dcp_sha256 [timing_run::sha256_file $output_dcp] \
    bitstream_sha256 [timing_run::sha256_file $output_bit]]
timing_run::write_manifest $output_manifest $manifest

puts "Refined checkpoint: $output_dcp"
puts "Refined bitstream: $output_bit"
puts "Refined manifest: $output_manifest"

close_design
exit
