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
# Resolve stage zero as literal register indices. NAME glob brackets would
# otherwise be character classes, and later synchronizer stages are local
# synchronous paths rather than the bounded asynchronous crossing.
proc video_cdc_first_gray_stage {cells} {
    set first {}
    foreach cell $cells {
        if {[regexp {dest_graysync_ff_reg\[0\]\[[0-9]+\]$} [get_property NAME $cell]]} {
            lappend first $cell
        }
    }
    return $first
}

proc video_cdc_check_bound {label sources targets expected source_clock destination_clock limit} {
    if {[llength $sources] != $expected || [llength $targets] != $expected} {
        error "Expected $expected $label source/target registers, got [llength $sources]/[llength $targets]"
    }
    foreach {cells clock} [list $sources $source_clock $targets $destination_clock] {
        set clocks [get_clocks -quiet -of_objects \
            [get_pins -of_objects $cells -filter {REF_PIN_NAME == C}]]
        if {[llength $clocks] != 1 || [get_property NAME $clocks] ne [get_property NAME $clock]} {
            error "Unexpected clock domain for $label: $clocks"
        }
    }
    # Vivado identifies a sequential timing startpoint by its clock pin,
    # including a datapath-only max-delay path launched from that register.
    set launch_pins [get_pins -of_objects $sources -filter {REF_PIN_NAME == C}]
    set capture_pins [get_pins -of_objects $targets -filter {REF_PIN_NAME == D}]
    if {[llength $launch_pins] != $expected || [llength $capture_pins] != $expected} {
        error "Missing $label launch/capture pins"
    }
    set paths [get_timing_paths -quiet -delay_type max -from $sources -to $capture_pins \
        -max_paths [expr {$expected + 1}] -nworst 1]
    if {[llength $paths] != $expected} {
        error "Expected $expected bounded $label paths, got [llength $paths]; check false-path precedence"
    }
    set checked_endpoints {}
    foreach path $paths {
        set endpoint [get_property ENDPOINT_PIN $path]
        set startpoint [get_property STARTPOINT_PIN $path]
        set requirement [get_property REQUIREMENT $path]
        set slack [get_property SLACK $path]
        if {[lsearch -exact $launch_pins $startpoint] < 0 ||
            [lsearch -exact $capture_pins $endpoint] < 0 ||
            [lsearch -exact $checked_endpoints $endpoint] >= 0 ||
            ![string is double -strict $requirement] ||
            abs($requirement - $limit) > 0.0005 ||
            ![string is double -strict $slack] || $slack eq "inf" || $slack < 0.0} {
            error "Missing or failing $label bound: $endpoint ($requirement ns, $slack ns slack)"
        }
        lappend checked_endpoints $endpoint
    }
    puts "PASS: $expected $label bounds are active and met (limit $limit ns)."
}

# Keep the existing 13 bounds in each FIFO direction. Other functional CDCs
# now share these clock domains and must not enter this pointer-only count.
foreach {source destination pointer} [list \
        $checked_fabric_clock $checked_pixel_clock wr_pntr_cdc_inst \
        $checked_pixel_clock $checked_fabric_clock rd_pntr_cdc_inst] {
    set prefix "*video_top_i/fb_reader_i/*gen_cdc_pntr.$pointer"
    set sources [get_cells -hierarchical -filter "NAME =~ $prefix/src_gray_ff_reg*"]
    set targets [video_cdc_first_gray_stage \
        [get_cells -hierarchical -filter "NAME =~ $prefix/dest_graysync_ff_reg*"]]
    video_cdc_check_bound "FIFO $pointer" $sources $targets 13 $source $destination \
        [get_property PERIOD $source]
}

set checked_mask_gray_sources [get_cells -hierarchical -filter \
    {NAME =~ *video_top_i/mask_config_i/frame_id_cdc/src_gray_ff_reg*}]
set checked_mask_gray_targets [video_cdc_first_gray_stage \
    [get_cells -hierarchical -filter \
        {NAME =~ *video_top_i/mask_config_i/frame_id_cdc/dest_graysync_ff_reg*}]]
video_cdc_check_bound "mask frame-token Gray" \
    $checked_mask_gray_sources $checked_mask_gray_targets 8 \
    $checked_pixel_clock $checked_fabric_clock [get_property PERIOD $checked_pixel_clock]

# The 616-bit mailbox has 446 live bits: mode/count (6), viewport (48),
# exclusions (384), and the returned frame token (8). Reserved bits are
# removed by synthesis. XPM handshake buses wider than 100 live bits use
# min(source period, destination period), not the four-stage latency bound.
set checked_mask_mailbox_sources [get_cells -hierarchical -filter \
    {NAME =~ *video_top_i/mask_config_i/config_mailbox/src_hsdata_ff_reg*}]
set checked_mask_mailbox_targets [get_cells -hierarchical -filter \
    {NAME =~ *video_top_i/mask_config_i/config_mailbox/dest_hsdata_ff_reg*}]
set checked_mask_mailbox_limit [expr {min([get_property PERIOD $checked_fabric_clock], \
                                         [get_property PERIOD $checked_pixel_clock])}]
video_cdc_check_bound "mask mailbox data" \
    $checked_mask_mailbox_sources $checked_mask_mailbox_targets 446 \
    $checked_fabric_clock $checked_pixel_clock $checked_mask_mailbox_limit

puts "PASS: six Apple capture, 26 FIFO Gray, eight mask frame-token, and 446 mask mailbox bounds are active and met."

# Keep Tcl control flow here: Vivado does not support an if guard in XDC.
# Port attributes survive retiming, but check the actual package-facing FFs
# too so a missing or renamed register cannot silently lose IOB packing.
set checked_dvi_ports [get_ports {dvi_red[*] dvi_grn[*] dvi_blu[*] dvi_de dvi_hsync dvi_vsync}]
if {[llength $checked_dvi_ports] != 19} {error "Expected all 19 registered DVI data and sync ports"}
set checked_dvi_registers {}
foreach port $checked_dvi_ports {
    if {![get_property IOB $port]} {error "Missing DVI port IOB property: $port"}
    set buffer [get_cells -of_objects [get_pins -leaf -of_objects \
        [get_nets -of_objects $port] -filter {DIRECTION == OUT}]]
    if {[llength $buffer] != 1 || [get_property REF_NAME $buffer] ne "OBUF"} {
        error "Expected one DVI output buffer: $port"
    }
    set driver [get_cells -of_objects [get_pins -leaf -of_objects \
        [get_nets -of_objects [get_pins -of_objects $buffer -filter {REF_PIN_NAME == I}]] \
        -filter {DIRECTION == OUT}]]
    set package [get_package_pins -of_objects $port]
    set site [string map {IOB_ OLOGIC_} [get_sites -of_objects $package]]
    if {[llength $package] != 1 || [llength $site] != 1 ||
        [llength $driver] != 1 || [get_property REF_NAME $driver] ne "FDRE" ||
        ![get_property IOB $driver] || [get_property LOC $driver] ne $site ||
        [get_property BEL $driver] ne "OLOGICE2.OUTFF" ||
        [lsearch -exact $checked_dvi_registers $driver] >= 0} {
        error "DVI output is not packed into its distinct package-facing IOB register: $port $driver"
    }
    lappend checked_dvi_registers $driver
}
puts "PASS: all 19 distinct DVI output registers retain IOB properties and package-matched OLOGIC.OUTFF placement."
