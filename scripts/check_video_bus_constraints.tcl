# Run on an open implemented design. A passing WNS alone does not prove
# these asynchronous route bounds survived higher-priority exceptions.
set checked_fabric_clock [get_clocks clk_out1_zynq_ps_bd_clk_wiz_1_0]
set checked_pixel_clock [get_clocks clk_out1_zynq_ps_bd_clk_wiz_0_0]
if {[llength $checked_fabric_clock] != 1 ||
    [llength $checked_pixel_clock] != 1} {
    error "Expected one fabric clock and one pixel clock for video CDC checks"
}
set checked_video_mode_sources [get_cells -hierarchical -filter \
    {NAME =~ *video_top_i/mode_control_i/active_mode_reg*}]
if {[llength $checked_video_mode_sources] == 0} {
    error "Missing video mode CDC source registers"
}
set checked_video_rate_d [get_pins -of_objects \
    [get_cells -hierarchical -filter \
        {NAME =~ *video_top_i/cdc_apple_video_mode_50hz_i/sync_meta_reg}] \
    -filter {REF_PIN_NAME == D}]
if {[llength $checked_video_rate_d] != 1} {
    error "Expected one Apple video-rate CDC input"
}
set checked_capture_pins [get_pins -of_objects \
    [get_cells -hierarchical -filter \
        {NAME =~ *onee_mode_safety_guard_i/generate_raw_transition_capture*.raw_transition_latched_reg*}] \
    -filter {REF_PIN_NAME == PRE}]
if {[llength $checked_capture_pins] != 6} {
    error "Expected six raw Apple capture PRE pins"
}
set checked_capture_ports [get_ports {a2fpga_clk a2fpga_7m a2fpga_q3 \
    a2fpga_m2sel a2fpga_m2b0 a2fpga_devsel_n}]
if {[llength $checked_capture_ports] != 6} {
    error "Expected six raw Apple capture input ports"
}
foreach pin $checked_capture_pins {
    set paths [get_timing_paths -quiet -from $checked_capture_ports -to $pin -max_paths 1]
    if {[llength $paths] != 1} {error "Missing raw capture path: $pin"}
    set requirement [get_property REQUIREMENT $paths]
    set slack [get_property SLACK $paths]
    if {![string is double -strict $requirement] ||
        abs($requirement - 5.0) > 0.0005 ||
        ![string is double -strict $slack] || $slack eq "inf" || $slack < 0.0} {
        error "Raw capture bound is missing or fails: $pin ($requirement ns, $slack ns slack)"
    }
}
foreach {source destination pointer} [list \
        $checked_fabric_clock $checked_pixel_clock wr_pntr_cdc_inst \
        $checked_pixel_clock $checked_fabric_clock rd_pntr_cdc_inst] {
    set paths [get_timing_paths -quiet -from $source -to $destination \
        -max_paths 1000 -nworst 1]
    if {[llength $paths] != 13} {
        error "Expected exactly 13 bounded $pointer paths, got [llength $paths]"
    }
    set limit [get_property PERIOD $source]
    foreach path $paths {
        set endpoint [get_property ENDPOINT_PIN $path]
        set requirement [get_property REQUIREMENT $path]
        set slack [get_property SLACK $path]
        if {![string match "*gen_cdc_pntr.$pointer/dest_graysync_ff_reg*" $endpoint] ||
            ![string is double -strict $requirement] ||
            abs($requirement - $limit) > 0.0005 ||
            ![string is double -strict $slack] || $slack eq "inf" || $slack < 0.0} {
            error "Missing or failing FIFO Gray bound: $endpoint ($requirement ns, $slack ns slack)"
        }
    }
}
puts "PASS: six Apple capture bounds and 26 FIFO Gray-pointer bounds are active and met."
