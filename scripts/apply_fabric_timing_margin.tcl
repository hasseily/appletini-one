# Make implementation optimize the fabric and Apple direction outputs for
# the release margin. clear_fabric_timing_margin.tcl restores the exact nominal
# constraints before any signoff report or exported artifact.
proc appletini_check_bus_output_limits {fabric_clock dir_limit phi_limit} {
    set dir_ports [get_ports -quiet {a2fpga_dir_a a2fpga_dir_d}]
    set phi0_port [get_ports -quiet a2fpga_clk]
    if {[llength $dir_ports] != 2 || [llength $phi0_port] != 1} {
        error "Expected both Apple direction ports and the PHI0 input."
    }
    set path_specs {}
    foreach dir_port $dir_ports {
        lappend path_specs [list $fabric_clock $dir_port $dir_limit]
    }
    lappend path_specs [list $phi0_port [get_ports a2fpga_dir_d] $phi_limit]
    foreach spec $path_specs {
        lassign $spec from to expected
        set path [get_timing_paths -quiet -delay_type max \
            -from $from -to $to -max_paths 1]
        if {[llength $path] != 1} {
            error "No timing path from $from to $to."
        }
        set requirement [get_property REQUIREMENT $path]
        if {![string is double -strict $requirement] ||
            abs(double($requirement) - $expected) > 0.0005} {
            error "Expected $expected ns from $from to $to, got $requirement ns."
        }
    }
}

set fabric_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_1_0]
if {[llength $fabric_clock] != 1} {
    error "Expected exactly one 133 MHz fabric clock."
}
appletini_check_bus_output_limits $fabric_clock 10.000 8.000

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
# These match the selectors and exception types in appletini_yarz.xdc.
# The port-to-port exception is more specific than the two direction limits.
set_max_delay 9.800 -to [get_ports {a2fpga_dir_a a2fpga_dir_d}]
set_max_delay -datapath_only 7.800 \
    -from [get_ports a2fpga_clk] -to [get_ports a2fpga_dir_d]
appletini_check_bus_output_limits $fabric_clock 9.800 7.800

set fabric_path [get_timing_paths -quiet -delay_type max \
    -from $fabric_clock -to $fabric_clock -max_paths 1]
set user_uncertainty [get_property USER_UNCERTAINTY $fabric_path]
if {![string is double -strict $user_uncertainty] ||
    abs(double($user_uncertainty) - 0.200) > 0.0005} {
    error "Fabric implementation margin did not apply: $user_uncertainty ns."
}
puts "Applied temporary fabric setup margin: $user_uncertainty ns"
puts "Applied temporary Apple output limits: direction 9.800 ns, PHI0 release 7.800 ns"
