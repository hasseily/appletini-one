# Make implementation optimize the 133 MHz fabric for the same setup margin
# required by the release gate. This is temporary implementation pressure, not
# a final timing exception; clear_fabric_timing_margin.tcl removes it.
set fabric_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_1_0]
if {[llength $fabric_clock] != 1} {
    error "Expected exactly one 133 MHz fabric clock."
}

set fabric_path [get_timing_paths -quiet -delay_type max \
    -from $fabric_clock -to $fabric_clock -max_paths 1]
if {[llength $fabric_path] != 1} {
    error "No fabric setup path found before applying timing margin."
}
set user_uncertainty [get_property USER_UNCERTAINTY $fabric_path]
if {$user_uncertainty ne "" &&
    (![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty)) > 0.0005)} {
    error "Refusing to replace fabric user uncertainty: $user_uncertainty ns."
}

set_clock_uncertainty -setup 0.200 $fabric_clock

set fabric_path [get_timing_paths -quiet -delay_type max \
    -from $fabric_clock -to $fabric_clock -max_paths 1]
set user_uncertainty [get_property USER_UNCERTAINTY $fabric_path]
if {![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty) - 0.200) > 0.0005} {
    error "Fabric implementation margin did not apply: $user_uncertainty ns."
}
puts "Applied temporary fabric setup margin: $user_uncertainty ns"
