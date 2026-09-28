# Run one pixel-only repair after routing, then restore nominal constraints
# before Vivado saves the routed checkpoint and writes the bitstream.
set fabric_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_1_0]
set pixel_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_0_0]
if {[llength $fabric_clock] != 1 || [llength $pixel_clock] != 1} {
    error "Expected exactly one fabric clock and one pixel clock."
}
foreach finish_clock [concat $fabric_clock $pixel_clock] expected {0.200 0.000} {
    set finish_path [get_timing_paths -quiet -delay_type max \
        -from $finish_clock -to $finish_clock -max_paths 1]
    if {[llength $finish_path] != 1} {
        error "No setup path for $finish_clock before video timing repair."
    }
    set user_uncertainty [get_property USER_UNCERTAINTY $finish_path]
    if {$user_uncertainty eq ""} {set user_uncertainty 0.000}
    if {![string is double -strict $user_uncertainty] ||
        abs(double($user_uncertainty) - $expected) > 0.0005} {
        error "Expected $expected ns $finish_clock before video timing repair, got $user_uncertainty ns."
    }
}

set_clock_uncertainty -setup 0.200 $pixel_clock
set finish_path [get_timing_paths -quiet -delay_type max \
    -from $pixel_clock -to $pixel_clock -max_paths 1]
if {[llength $finish_path] != 1} {
    error "No setup path for $pixel_clock after applying video timing margin."
}
set user_uncertainty [get_property USER_UNCERTAINTY $finish_path]
if {![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty) - 0.200) > 0.0005} {
    error "$pixel_clock implementation margin did not apply: $user_uncertainty ns."
}

phys_opt_design -routing_opt -critical_cell_opt -critical_pin_opt \
    -path_groups clk_out1_zynq_ps_bd_clk_wiz_0_0
source [file join [file dirname [info script]] clear_fabric_timing_margin.tcl]
