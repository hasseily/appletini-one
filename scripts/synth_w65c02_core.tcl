# Out-of-context resource and timing characterization for the standalone core.
set output_dir [file normalize "build/w65c02_synth"]
file mkdir $output_dir

read_verilog -sv [file normalize "hdl/apple/w65c02_core.sv"]
synth_design -top w65c02_core -part xc7z020clg484-2 -mode out_of_context

# Appletini's fast PL domain is 133.333 MHz. The core is normally advanced
# with its enable input, so this constrains the worst-case every-clock path.
create_clock -name core_clk -period 7.500 [get_ports clk]

opt_design
place_design
phys_opt_design
route_design

report_utilization -file [file join $output_dir utilization.rpt]
report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose -file [file join $output_dir timing_summary.rpt]
report_route_status -file [file join $output_dir route_status.rpt]
write_checkpoint -force [file join $output_dir w65c02_core_routed.dcp]
