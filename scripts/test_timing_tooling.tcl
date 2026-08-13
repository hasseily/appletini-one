source [file join [file dirname [info script]] timing_run_helpers.tcl]

proc require_equal {actual expected message} {
    if {$actual ne $expected} {
        error "$message: expected '$expected', got '$actual'"
    }
}

set timing_fixture {
4. checking unconstrained_internal_endpoints (0)
    WNS(ns) TNS(ns) endpoints total WHS(ns) THS(ns) endpoints total WPWS(ns) TPWS(ns) endpoints total
      0.312 0.000 0 58992 0.061 0.000 0 58925 0.265 0.000 0 20752
}
set timing [timing_run::parse_timing_summary $timing_fixture]
require_equal [dict get $timing wns_ns] 0.312 "WNS parser"
require_equal [dict get $timing whs_ns] 0.061 "WHS parser"
require_equal [dict get $timing setup_failing_endpoints] 0 \
    "Setup failing-endpoint parser"
require_equal [dict get $timing hold_failing_endpoints] 0 \
    "Hold failing-endpoint parser"
require_equal [dict get $timing pulse_width_failing_endpoints] 0 \
    "Pulse-width failing-endpoint parser"
require_equal [dict get $timing unconstrained_internal_endpoints] 0 \
    "Unconstrained endpoint parser"

set utilization_fixture {
| Slice LUTs          | 30465 | 1 | 0 | 53200 | 57.27 |
|   LUT as Logic      | 30264 | 1 | 0 | 53200 | 56.89 |
|   LUT as Memory     | 201   | 0 | 0 | 17400 | 1.16 |
|     LUT as Shift Register | 11 | 0 | | | |
| Slice Registers     | 20153 | 0 | 0 | 106400 | 18.94 |
| F7 Muxes            | 1487  | 0 | 0 | 26600 | 5.59 |
| F8 Muxes            | 441   | 0 | 0 | 13300 | 3.32 |
| Unique Control Sets | 1001  |   | 0 | 13300 | 7.53 |
| Slice               | 10223 | 0 | 0 | 13300 | 76.86 |
| Block RAM Tile      | 74    | 0 | 0 | 140 | 52.86 |
|     RAMB36E1 only   | 69    | | | | |
|     RAMB18E1 only   | 10    | | | | |
| CARRY4              | 2871  | | | | |
| DSPs                | 6     | 0 | 0 | 220 | 2.73 |
}
set utilization [timing_run::parse_utilization $utilization_fixture]
require_equal [dict get $utilization slices] 10223 "Slice parser"
require_equal [dict get $utilization luts] 30465 "LUT parser"
require_equal [dict get $utilization control_sets] 1001 "Control-set parser"
require_equal [dict get $utilization lut_logic] 30264 "Logic-LUT parser"
require_equal [dict get $utilization ramb36] 69 "RAMB36 parser"
require_equal [dict get $utilization carry4s] 2871 "CARRY4 parser"

set route [timing_run::parse_route_status {
    # of nets with routing errors.................. : 0 :
}]
require_equal [dict get $route route_status] PASS "Route-status parser"
require_equal [dict get $route route_errors] 0 "Route-error parser"

set skew [timing_run::parse_bus_skew {
1 foo Slow 6.733 1.211 5.522
2 foo Slow 6.733 7.000 -0.267
}]
require_equal [dict get $skew bus_skew_status] FAIL "Bus-skew status parser"
require_equal [dict get $skew bus_skew_wns_ns] -0.267 "Bus-skew slack parser"

set signoff [dict create \
    status exported build_mode full git_dirty 0 \
    rescue_used 0 incremental_reference "" incremental_reference_sha256 "" \
    vivado_version 2025.2 device_part xc7z020clg484-2 speed_grade -2 \
    seed_control default synth_strategy synth synth_retiming auto \
    control_set_opt_threshold auto impl_strategy impl \
    place_directive Explore phys_opt_directive Explore \
    route_directive {directive=Explore;more_options=-tns_cleanup} \
    post_route_phys_opt_directive {enabled=1;directive=Explore} jobs 8 \
    wns_ns 0.312 tns_ns 0.000 whs_ns 0.061 ths_ns 0.000 \
    setup_failing_endpoints 0 hold_failing_endpoints 0 \
    pulse_width_failing_endpoints 0 \
    wpws_ns 0.265 tpws_ns 0.000 unconstrained_internal_endpoints 0 \
    route_errors 0 route_status PASS bus_skew_status PASS \
    missing_constraint_objects 0 \
    candidate_dcp_sha256 [string repeat a 64] \
    bitstream_sha256 [string repeat b 64] \
    xsa_sha256 [string repeat c 64]]
timing_run::validate_signoff_manifest $signoff
set rescued $signoff
dict set rescued rescue_used 1
if {![catch {timing_run::validate_signoff_manifest $rescued}]} {
    error "Promotion policy accepted a rescued build."
}
dict set signoff whs_ns -0.001
if {![catch {timing_run::validate_signoff_manifest $signoff}]} {
    error "Promotion policy accepted negative hold slack."
}

set temp_channel [file tempfile temp_path]
close $temp_channel
timing_run::write_manifest $temp_path [dict create z 2 a 1]
set round_trip [timing_run::read_manifest $temp_path]
file delete -force $temp_path
require_equal [dict get $round_trip a] 1 "Manifest round trip"
require_equal [dict get $round_trip z] 2 "Manifest round trip"
require_equal [timing_run::validate_build_id 20260812T175800Z-a3d71d3f-full] 1 \
    "Build-ID validator"
require_equal [timing_run::validate_build_id ../bad] 0 "Build-ID traversal rejection"

set all_hardware_tests \
    "boot-menu,disk-ii,smartport,vtw,mb-audit,linear-overlay,sdd,uthernet,ssc,reset"
timing_run::validate_hardware_tests $all_hardware_tests
if {![catch {timing_run::validate_hardware_tests \
    "boot-menu,disk-ii,smartport,vtw,mb-audit"}]} {
    error "Hardware test policy accepted an incomplete list."
}

set first_flow [dict create vivado_version 2025.2 route_directive Explore]
set second_flow $first_flow
timing_run::require_matching_manifest_values \
    $first_flow $second_flow {vivado_version route_directive}
dict set second_flow route_directive ExtraNetDelay_high
if {![catch {timing_run::require_matching_manifest_values \
        $first_flow $second_flow {vivado_version route_directive}}]} {
    error "Promotion policy accepted different flow settings."
}

set temp_channel [file tempfile timing_history_root]
close $temp_channel
file delete -force $timing_history_root
file mkdir $timing_history_root
set first_id 20260812T175800Z-a3d71d3f-full
set second_id 20260812T180800Z-a3d71d3f-full
foreach item [list \
    [list $first_id 2026-08-12T17:58:00Z] \
    [list $second_id 2026-08-12T18:08:00Z]] {
    lassign $item build_id utc_start
    set run_dir [file join $timing_history_root $build_id]
    file mkdir $run_dir
    timing_run::write_manifest [file join $run_dir manifest.txt] [dict create \
        build_id $build_id build_mode full git_sha [string repeat a 40] \
        utc_start $utc_start]
}
timing_run::require_latest_full_pair \
    $timing_history_root $first_id $second_id
set third_id 20260812T181800Z-bbbbbbbb-full
set third_dir [file join $timing_history_root $third_id]
file mkdir $third_dir
timing_run::write_manifest [file join $third_dir manifest.txt] [dict create \
    build_id $third_id build_mode full git_sha [string repeat b 40] \
    utc_start 2026-08-12T18:18:00Z]
if {![catch {timing_run::require_latest_full_pair \
    $timing_history_root $first_id $second_id}]} {
    error "Promotion history accepted a pair before the latest full build."
}
file delete -force [file join $third_dir manifest.txt]
if {![catch {timing_run::require_latest_full_pair \
    $timing_history_root $second_id $third_id}]} {
    error "Promotion history accepted a full build with no manifest."
}
file delete -force $timing_history_root

set temp_channel [file tempfile hash_fixture]
puts -nonewline $temp_channel "timing artifact"
close $temp_channel
set expected_hash [timing_run::sha256_file $hash_fixture]
timing_run::require_file_hash $hash_fixture $expected_hash "Hash fixture"
if {![catch {timing_run::require_file_hash \
    $hash_fixture [string repeat 0 64] "Hash fixture"}]} {
    error "Artifact check accepted a bad hash."
}
file delete -force $hash_fixture

set missing [timing_run::count_missing_constraint_objects {
WARNING: No valid object(s) found for set_false_path.
WARNING: get_pins matched no objects.
INFO: normal line
}]
require_equal $missing 2 "Missing constraint-object warning parser"

puts "PASS: timing tooling helpers"
