# Implementation only: register with USED_IN_SYNTHESIS=false and
# PROCESSING_ORDER=LATE after the linked IP clocks are available.
# Keep generated FIFO Gray-pointer maximum delays and bus-skew checks active.
# Generated XPM exceptions cover pulse, underrun and FIFO reset crossings;
# appletini_yarz.xdc covers reset synchronizer CLR pins.
# active_mode changes while scanout is reset and remains stable through
# synchronized reset release. Only these source registers bypass CDC timing.
set_false_path -from [get_cells -hierarchical -filter \
    {NAME =~ *video_top_i/mode_control_i/active_mode_reg*}] \
    -to [get_clocks clk_out1_zynq_ps_bd_clk_wiz_0_0]

# The live Apple video-rate flag enters one two-flop synchronizer.
set_false_path -from [get_clocks clk_out1_zynq_ps_bd_clk_wiz_1_0] \
    -to [get_pins -of_objects \
        [get_cells -hierarchical -filter \
            {NAME =~ *video_top_i/cdc_apple_video_mode_50hz_i/sync_meta_reg}] \
        -filter {REF_PIN_NAME == D}]
