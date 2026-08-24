# Place and route the schematic-derived SSI-263 audio engine by itself.
# This is a fast timing check, not a full Appletini card build.

set script_dir [file normalize [file dirname [info script]]]
set repo_dir [file normalize [file join $script_dir ..]]
set out_dir [file join $repo_dir build ssi263_sc02_audio_ooc_route]
set rtl_file [file join $repo_dir hdl apple ssi263_sc02_audio.sv]

file mkdir $out_dir

read_verilog -sv $rtl_file
synth_design -top ssi263_sc02_audio \
    -part xc7z020clg484-2 \
    -mode out_of_context

# The generated PS fabric clock is 133,333,344 Hz.
create_clock -name fabric_clk -period 7.500 [get_ports clk]

opt_design
place_design
phys_opt_design
route_design

report_utilization -hierarchical \
    -file [file join $out_dir utilization_hierarchical.rpt]
report_timing_summary -delay_type max -max_paths 20 \
    -file [file join $out_dir timing_summary_7p500ns.rpt]
report_timing -delay_type max -max_paths 20 -nworst 1 -path_type full \
    -file [file join $out_dir timing_paths_7p500ns.rpt]
report_route_status -file [file join $out_dir route_status.rpt]
write_checkpoint -force [file join $out_dir ssi263_sc02_audio_routed.dcp]

set worst_path [lindex [get_timing_paths -delay_type max -max_paths 1] 0]
set dsp_count [llength [get_cells -hier -filter {REF_NAME == DSP48E1}]]

set summary_file [file join $out_dir audit_summary.txt]
set summary [open $summary_file w]
puts $summary "tool=[version -short]"
puts $summary "part=xc7z020clg484-2"
puts $summary "top=ssi263_sc02_audio"
puts $summary "rtl=$rtl_file"
puts $summary "clock_period_ns=7.500"
puts $summary "dsp48e1=$dsp_count"
puts $summary "route_status=completed"
puts $summary "wns_ns=[get_property SLACK $worst_path]"
puts $summary "datapath_delay_ns=[get_property DATAPATH_DELAY $worst_path]"
puts $summary "startpoint=[get_property STARTPOINT_PIN $worst_path]"
puts $summary "endpoint=[get_property ENDPOINT_PIN $worst_path]"
close $summary

puts "AUDIT_SUMMARY $summary_file"
puts "AUDIT_DSP48E1 $dsp_count"
puts "AUDIT_ROUTE_STATUS completed"
puts "AUDIT_WNS_NS [get_property SLACK $worst_path]"
puts "AUDIT_DATAPATH_DELAY_NS [get_property DATAPATH_DELAY $worst_path]"
puts "AUDIT_STARTPOINT [get_property STARTPOINT_PIN $worst_path]"
puts "AUDIT_ENDPOINT [get_property ENDPOINT_PIN $worst_path]"
