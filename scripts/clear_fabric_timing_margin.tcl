# Restore nominal constraints after the fabric-only flow or routed timing repair.
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
set pixel_clock [get_clocks -quiet clk_out1_zynq_ps_bd_clk_wiz_0_0]
if {[llength $fabric_clock] != 1 || [llength $pixel_clock] != 1} {
    error "Expected exactly one fabric clock and one pixel clock."
}
appletini_check_bus_output_limits $fabric_clock 9.800 7.800

set margin_clocks [concat $fabric_clock $pixel_clock]
foreach margin_clock $margin_clocks {
    set margin_path [get_timing_paths -quiet -delay_type max \
        -from $margin_clock -to $margin_clock -max_paths 1]
    if {[llength $margin_path] != 1} {
        error "No setup path for $margin_clock before clearing timing margin."
    }
    set user_uncertainty [get_property USER_UNCERTAINTY $margin_path]
    if {$margin_clock eq $pixel_clock && $user_uncertainty eq ""} {
        set user_uncertainty 0.000
    }
    if {$margin_clock eq $fabric_clock} {
        if {![string is double -strict $user_uncertainty] ||
            abs(double($user_uncertainty) - 0.200) > 0.0005} {
            error "Expected 0.200 ns $margin_clock implementation margin, got $user_uncertainty ns."
        }
    } elseif {![string is double -strict $user_uncertainty] ||
              (abs(double($user_uncertainty)) > 0.0005 &&
               abs(double($user_uncertainty) - 0.200) > 0.0005)} {
        error "Expected 0.000 or 0.200 ns $margin_clock implementation margin, got $user_uncertainty ns."
    }
}

set_clock_uncertainty -setup 0.0 $margin_clocks
set_max_delay 10.000 -to [get_ports {a2fpga_dir_a a2fpga_dir_d}]
set_max_delay -datapath_only 8.000 \
    -from [get_ports a2fpga_clk] -to [get_ports a2fpga_dir_d]
appletini_check_bus_output_limits $fabric_clock 10.000 8.000

foreach margin_clock $margin_clocks {
    set margin_path [get_timing_paths -quiet -delay_type max \
        -from $margin_clock -to $margin_clock -max_paths 1]
    if {[llength $margin_path] != 1} {
        error "No setup path for $margin_clock after clearing timing margin."
    }
    set user_uncertainty [get_property USER_UNCERTAINTY $margin_path]
    if {$user_uncertainty eq ""} {set user_uncertainty 0.000}
    if {![string is double -strict $user_uncertainty] ||
        abs(double($user_uncertainty)) > 0.0005} {
        error "$margin_clock implementation margin did not clear: $user_uncertainty ns."
    }
    puts "Cleared temporary setup margin from $margin_clock: $user_uncertainty ns"
}
puts "Restored nominal Apple output limits: direction 10.000 ns, PHI0 release 8.000 ns"
