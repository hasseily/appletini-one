# Validate both hooks against an existing design without changing placement,
# routing, the checkpoint, or any exported hardware.
if {$argc != 1} {
    error "Usage: test_timing_margin_checkpoint.tcl ROUTED_DCP"
}
set input_dcp [file normalize [lindex $argv 0]]
if {![file isfile $input_dcp]} {
    error "Input checkpoint does not exist: $input_dcp"
}
set script_dir [file dirname [file normalize [info script]]]
open_checkpoint $input_dcp

set before_setup [get_property SLACK \
    [get_timing_paths -quiet -delay_type max -max_paths 1]]
set before_hold [get_property SLACK \
    [get_timing_paths -quiet -delay_type min -max_paths 1]]
source [file join $script_dir apply_fabric_timing_margin.tcl]
source [file join $script_dir clear_fabric_timing_margin.tcl]
set after_setup [get_property SLACK \
    [get_timing_paths -quiet -delay_type max -max_paths 1]]
set after_hold [get_property SLACK \
    [get_timing_paths -quiet -delay_type min -max_paths 1]]
if {abs(double($after_setup) - $before_setup) > 0.0005 ||
    abs(double($after_hold) - $before_hold) > 0.0005} {
    error "Hook round trip changed nominal timing: setup $before_setup -> $after_setup; hold $before_hold -> $after_hold"
}
puts "PASS: timing margin checkpoint round trip; nominal setup=$after_setup ns hold=$after_hold ns"
close_design
