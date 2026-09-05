# Remove the temporary implementation margin before the final timing report,
# checkpoint, bitstream, and hardware-platform export.
set fabric_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_1_0]
if {[llength $fabric_clock] != 1} {
    error "Expected exactly one 133 MHz fabric clock."
}

set fabric_path [get_timing_paths -quiet -delay_type max \
    -from $fabric_clock -to $fabric_clock -max_paths 1]
if {[llength $fabric_path] != 1} {
    error "No fabric setup path found before clearing timing margin."
}
set user_uncertainty [get_property USER_UNCERTAINTY $fabric_path]
if {![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty) - 0.200) > 0.0005} {
    error "Expected 0.200 ns fabric implementation margin, got $user_uncertainty ns."
}

set_clock_uncertainty -setup 0.0 $fabric_clock

set fabric_path [get_timing_paths -quiet -delay_type max \
    -from $fabric_clock -to $fabric_clock -max_paths 1]
set user_uncertainty [get_property USER_UNCERTAINTY $fabric_path]
if {$user_uncertainty eq ""} {
    set user_uncertainty 0.000
}
if {![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty)) > 0.0005} {
    error "Fabric implementation margin did not clear: $user_uncertainty ns."
}
puts "Cleared temporary fabric setup margin: $user_uncertainty ns"
