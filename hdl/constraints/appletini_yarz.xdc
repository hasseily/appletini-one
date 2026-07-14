################################################################################
# Appletini One Pin Constraints
#
# Xilinx Zynq-7020 CLG484 Package
# Apple //e Expansion Card - Multi-Card Emulator
#
# Generated from: Xilinx_Zynq-7020_FormalTruthTable.csv
# Device: xc7z020clg484-1
#
# Note: PS pins (MIO, DDR) are configured by the Zynq PS IP and should not
# be constrained here. Only PL (Programmable Logic) pins are constrained.
################################################################################

################################################################################
# Apple //e Bus Interface - Bank 33
# All signals interface via level translators (bidirectional) or open-collector
################################################################################

## Apple //e Control Signals - Assert (Output from FPGA)
set_property PACKAGE_PIN U19 [get_ports a2ctrl_irq_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_irq_n]

set_property PACKAGE_PIN T21 [get_ports a2ctrl_nmi_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_nmi_n]

set_property PACKAGE_PIN U21 [get_ports a2ctrl_rdy_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_rdy_n]

set_property PACKAGE_PIN T22 [get_ports a2ctrl_dma_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_dma_n]

set_property PACKAGE_PIN W22 [get_ports a2ctrl_inh_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_inh_n]

set_property PACKAGE_PIN W20 [get_ports a2ctrl_reset_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2ctrl_reset_n]

## Apple //e Control Signals - Observe (Input to FPGA)
set_property PACKAGE_PIN U22 [get_ports a2fpga_dma_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_dma_n]

set_property PACKAGE_PIN V22 [get_ports a2fpga_rdy_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_rdy_n]

set_property PACKAGE_PIN W21 [get_ports a2fpga_inh_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_inh_n]

set_property PACKAGE_PIN U20 [get_ports a2fpga_nmi_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_nmi_n]

set_property PACKAGE_PIN V20 [get_ports a2fpga_reset_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_reset_n]

set_property PACKAGE_PIN V19 [get_ports a2fpga_irq_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_irq_n]

## Apple //e Bus Transceiver Control
set_property PACKAGE_PIN V18 [get_ports a2fpga_oe_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_oe_n]

set_property PACKAGE_PIN Y20 [get_ports a2fpga_oe_n_aux]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_oe_n_aux]

set_property PACKAGE_PIN U17 [get_ports a2fpga_dir_a]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_dir_a]

set_property PACKAGE_PIN V17 [get_ports a2fpga_dir_d]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_dir_d]

## Apple //e Address Bus (Bidirectional)
set_property PACKAGE_PIN AA13 [get_ports {a2fpga_a[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[0]}]

set_property PACKAGE_PIN AB14 [get_ports {a2fpga_a[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[1]}]

set_property PACKAGE_PIN AA14 [get_ports {a2fpga_a[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[2]}]

set_property PACKAGE_PIN AB15 [get_ports {a2fpga_a[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[3]}]

set_property PACKAGE_PIN AB16 [get_ports {a2fpga_a[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[4]}]

set_property PACKAGE_PIN AA16 [get_ports {a2fpga_a[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[5]}]

set_property PACKAGE_PIN Y16 [get_ports {a2fpga_a[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[6]}]

set_property PACKAGE_PIN Y15 [get_ports {a2fpga_a[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[7]}]

set_property PACKAGE_PIN Y19 [get_ports {a2fpga_a[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[8]}]

set_property PACKAGE_PIN AB19 [get_ports {a2fpga_a[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[9]}]

set_property PACKAGE_PIN AA19 [get_ports {a2fpga_a[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[10]}]

set_property PACKAGE_PIN AB20 [get_ports {a2fpga_a[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[11]}]

set_property PACKAGE_PIN AB21 [get_ports {a2fpga_a[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[12]}]

set_property PACKAGE_PIN AB22 [get_ports {a2fpga_a[13]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[13]}]

set_property PACKAGE_PIN AA21 [get_ports {a2fpga_a[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[14]}]

set_property PACKAGE_PIN AA22 [get_ports {a2fpga_a[15]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_a[15]}]

## Apple //e Data Bus (Bidirectional)
set_property PACKAGE_PIN Y14 [get_ports {a2fpga_d[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[0]}]

set_property PACKAGE_PIN W15 [get_ports {a2fpga_d[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[1]}]

set_property PACKAGE_PIN W16 [get_ports {a2fpga_d[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[2]}]

set_property PACKAGE_PIN V15 [get_ports {a2fpga_d[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[3]}]

set_property PACKAGE_PIN V14 [get_ports {a2fpga_d[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[4]}]

set_property PACKAGE_PIN U16 [get_ports {a2fpga_d[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[5]}]

set_property PACKAGE_PIN U15 [get_ports {a2fpga_d[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[6]}]

set_property PACKAGE_PIN U14 [get_ports {a2fpga_d[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {a2fpga_d[7]}]

## Apple //e Bus Control
set_property PACKAGE_PIN Y21 [get_ports a2fpga_rdwr_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_rdwr_n]

## Apple //e Clock Inputs
set_property PACKAGE_PIN Y18 [get_ports a2fpga_clk]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_clk]

set_property PACKAGE_PIN AA18 [get_ports a2fpga_7m]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_7m]

set_property PACKAGE_PIN W17 [get_ports a2fpga_q3]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_q3]

## Apple //e Status/Select Signals
set_property PACKAGE_PIN W18 [get_ports a2fpga_m2b0]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_m2b0]

set_property PACKAGE_PIN AA17 [get_ports a2fpga_m2sel]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_m2sel]

set_property PACKAGE_PIN AB17 [get_ports a2fpga_devsel_n]
set_property IOSTANDARD LVCMOS33 [get_ports a2fpga_devsel_n]

################################################################################
# DVI Video Output - Bank 34
# Parallel video to TFP410 DVI transmitter
################################################################################

## DVI Clock and Control
set_property PACKAGE_PIN L21 [get_ports dvi_clk]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_clk]

set_property PACKAGE_PIN L22 [get_ports dvi_de]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_de]

set_property PACKAGE_PIN R19 [get_ports dvi_hsync]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_hsync]

set_property PACKAGE_PIN T19 [get_ports dvi_vsync]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_vsync]

## DVI Red Channel (5-bit)
set_property PACKAGE_PIN L18 [get_ports {dvi_red[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_red[0]}]

set_property PACKAGE_PIN K18 [get_ports {dvi_red[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_red[1]}]

set_property PACKAGE_PIN K20 [get_ports {dvi_red[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_red[2]}]

set_property PACKAGE_PIN K19 [get_ports {dvi_red[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_red[3]}]

set_property PACKAGE_PIN L19 [get_ports {dvi_red[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_red[4]}]

## DVI Green Channel (6-bit)
set_property PACKAGE_PIN P16 [get_ports {dvi_grn[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[0]}]

set_property PACKAGE_PIN N17 [get_ports {dvi_grn[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[1]}]

set_property PACKAGE_PIN R16 [get_ports {dvi_grn[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[2]}]

set_property PACKAGE_PIN R18 [get_ports {dvi_grn[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[3]}]

set_property PACKAGE_PIN T17 [get_ports {dvi_grn[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[4]}]

set_property PACKAGE_PIN T18 [get_ports {dvi_grn[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_grn[5]}]

## DVI Blue Channel (5-bit)
set_property PACKAGE_PIN P18 [get_ports {dvi_blu[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_blu[0]}]

set_property PACKAGE_PIN T16 [get_ports {dvi_blu[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_blu[1]}]

set_property PACKAGE_PIN P17 [get_ports {dvi_blu[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_blu[2]}]

set_property PACKAGE_PIN P15 [get_ports {dvi_blu[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_blu[3]}]

set_property PACKAGE_PIN N15 [get_ports {dvi_blu[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {dvi_blu[4]}]

## DVI I2C (DDC/EDID)
set_property PACKAGE_PIN L17 [get_ports dvi_scl]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_scl]

set_property PACKAGE_PIN M17 [get_ports dvi_sda]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_sda]

## DVI Hot Plug Detect
set_property PACKAGE_PIN K21 [get_ports dvi_hpd]
set_property IOSTANDARD LVCMOS33 [get_ports dvi_hpd]

################################################################################
# Audio Output - Bank 34
################################################################################

## I2S Audio
set_property PACKAGE_PIN M19 [get_ports audio_i2s_mclk]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2s_mclk]

set_property PACKAGE_PIN J15 [get_ports audio_i2s_lrck]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2s_lrck]

set_property PACKAGE_PIN K15 [get_ports audio_i2s_sdata]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2s_sdata]

set_property PACKAGE_PIN J16 [get_ports audio_i2s_bick]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2s_bick]

## SPDIF Audio
set_property PACKAGE_PIN J17 [get_ports audio_spdif_out]
set_property IOSTANDARD LVCMOS33 [get_ports audio_spdif_out]

################################################################################
# PSRAM - Bank 34
# Two 4-bit wide PSRAM chips (A and B) with shared clock and chip enable
################################################################################

## PSRAM Chip A Data
set_property -dict \
    {package_pin P21 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_a_io[0]}]
set_property -dict \
    {package_pin N20 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_a_io[1]}]
set_property -dict \
    {package_pin N19 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_a_io[2]}]
set_property -dict \
    {package_pin N22 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_a_io[3]}]

set_property -dict \
    {package_pin R21 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_b_io[0]}]
set_property -dict \
    {package_pin P20 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_b_io[1]}]
set_property -dict \
    {package_pin R20 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_b_io[2]}]
set_property -dict \
    {package_pin P22 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_b_io[3]}]

set_property -dict \
    {package_pin M21 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_ce_n}]

set_property -dict \
    {package_pin M22 IOSTANDARD LVCMOS33 PULLTYPE {} SLEW FAST DRIVE 8} [get_ports {psram_clk}]

# set_property PACKAGE_PIN P21 [get_ports {psram_a_io[0]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_a_io[0]}]

# set_property PACKAGE_PIN N20 [get_ports {psram_a_io[1]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_a_io[1]}]

# set_property PACKAGE_PIN N19 [get_ports {psram_a_io[2]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_a_io[2]}]

# set_property PACKAGE_PIN N22 [get_ports {psram_a_io[3]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_a_io[3]}]

# ## PSRAM Chip B Data
# set_property PACKAGE_PIN R21 [get_ports {psram_b_io[0]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_b_io[0]}]

# set_property PACKAGE_PIN P20 [get_ports {psram_b_io[1]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_b_io[1]}]

# set_property PACKAGE_PIN R20 [get_ports {psram_b_io[2]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_b_io[2]}]

# set_property PACKAGE_PIN P22 [get_ports {psram_b_io[3]}]
# set_property IOSTANDARD LVCMOS33 [get_ports {psram_b_io[3]}]

# ## PSRAM Control (Shared)
# set_property PACKAGE_PIN M21 [get_ports psram_ce_n]
# set_property IOSTANDARD LVCMOS33 [get_ports psram_ce_n]

# set_property PACKAGE_PIN M22 [get_ports psram_clk]
# set_property IOSTANDARD LVCMOS33 [get_ports psram_clk]

################################################################################
# Peripheral Interfaces - Bank 34
################################################################################

## RTC (Real-Time Clock)
set_property PACKAGE_PIN J21 [get_ports i2c0_rtc_int_n]
set_property IOSTANDARD LVCMOS33 [get_ports i2c0_rtc_int_n]

set_property PACKAGE_PIN J22 [get_ports i2c0_rtc_clkout]
set_property IOSTANDARD LVCMOS33 [get_ports i2c0_rtc_clkout]

## Temperature Sensor
set_property PACKAGE_PIN J20 [get_ports i2c0_temp_alert_n]
set_property IOSTANDARD LVCMOS33 [get_ports i2c0_temp_alert_n]

## Ethernet W5100S Parallel MCU Interface - Bank 35
set_property PACKAGE_PIN B19 [get_ports {eth_d[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[1]}]
set_property PACKAGE_PIN B20 [get_ports {eth_d[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[3]}]
set_property PACKAGE_PIN A21 [get_ports {eth_d[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[4]}]
set_property PACKAGE_PIN A22 [get_ports {eth_d[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[6]}]
set_property PACKAGE_PIN B21 [get_ports {eth_d[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[5]}]
set_property PACKAGE_PIN B22 [get_ports {eth_d[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[7]}]
set_property PACKAGE_PIN A18 [get_ports {eth_d[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[0]}]
set_property PACKAGE_PIN A19 [get_ports {eth_d[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_d[2]}]

set_property PACKAGE_PIN A16 [get_ports {eth_a[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_a[0]}]
set_property PACKAGE_PIN B16 [get_ports {eth_a[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {eth_a[1]}]
set_property PACKAGE_PIN A17 [get_ports eth_rd_n]
set_property IOSTANDARD LVCMOS33 [get_ports eth_rd_n]
set_property PACKAGE_PIN B17 [get_ports eth_wr_n]
set_property IOSTANDARD LVCMOS33 [get_ports eth_wr_n]
set_property PACKAGE_PIN B15 [get_ports eth_cs_n]
set_property IOSTANDARD LVCMOS33 [get_ports eth_cs_n]
set_property PACKAGE_PIN C22 [get_ports eth_rst_n]
set_property IOSTANDARD LVCMOS33 [get_ports eth_rst_n]
set_property PACKAGE_PIN C20 [get_ports eth_int_n]
set_property IOSTANDARD LVCMOS33 [get_ports eth_int_n]

## USB System Fault
set_property PACKAGE_PIN H15 [get_ports usb_sys_fault_n]
set_property IOSTANDARD LVCMOS33 [get_ports usb_sys_fault_n]

################################################################################
# Clock Constraints
################################################################################

# Apple PHI0, 7M, and Q3 are sampled bus inputs rather than design clocks.

## DVI Pixel Clock - 148.5 MHz (generated by clk_wiz_0 from PS FCLK2 = 150 MHz)
## This clock is generated internally by the MMCM for 1920×1080 @ 60Hz
## Period = 1/148.5MHz = 6.734 ns
## Overall ratio: (150/5) MHz × 49.5 / 10 = 148.5 MHz

## Audio MCLK from A2 board audio oscillator
create_clock -period 81.380 -name audio_mclk [get_ports audio_i2s_mclk]

## Audio BCLK (derived in PL as audio_mclk/4)
create_generated_clock -name audio_bclk \
    -source [get_ports audio_i2s_mclk] \
    -divide_by 4 \
    [get_pins -hierarchical *audio_bclk_div_cnt_reg\[1\]/Q]

## PS FCLK clocks are created by the PS7 IP XDC; refer to them by name
## (avoid redefining clocks here to prevent overrides)

## Clock Groups (asynchronous to each other)
## PS FCLK (clk_fpga_0) is async to the video pixel clock.
## Use clock names (not internal pin paths) because hierarchy/pin naming can vary
## after synthesis/opt and cause the exception to fail to bind.
set_clock_groups -quiet -asynchronous \
    -group {clk_out1_zynq_ps_bd_clk_wiz_0_0} \
    -group {clk_out1_zynq_ps_bd_clk_wiz_1_0}

# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet clk_fpga_0] \
#     -group [get_clocks -quiet clk_pixel_148mhz]
# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet clk_fpga_1] \
#     -group [get_clocks -quiet clk_pixel_148mhz]

## PS clocks async to each other
# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet clk_fpga_0] \
#     -group [get_clocks -quiet clk_fpga_1]

# ## Audio MCLK async to PS clocks
# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet audio_mclk] \
#     -group [get_clocks -quiet clk_fpga_0]

# ## Audio BCLK async to PS/video clocks
# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet audio_bclk] \
#     -group [get_clocks -quiet clk_fpga_0] \
#     -group [get_clocks -quiet clk_pixel_148mhz]

# ## Apple II bus clocks (PHI0/7M) are asynchronous to PL internal clocks.
# ## The Apple bus frontend uses explicit CDC for AXI<->PHI0 crossings.
# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet a2_phi0_clk] \
#     -group [get_clocks -quiet {clk_fpga_0 clk_fpga_1 clk_pixel_148mhz audio_bclk audio_mclk}]

# set_clock_groups -quiet -asynchronous \
#     -group [get_clocks -quiet a2_7m_clk] \
#     -group [get_clocks -quiet {clk_fpga_0 clk_fpga_1 clk_pixel_148mhz audio_bclk audio_mclk}]

#create_clock -name top_clk_133 -period 6.667 \
    {zynq_ps_i/zynq_ps_bd_i/processing_system7_0/inst/PS7_i/FCLKCLK[2]}

create_generated_clock -name psram_clk_out \
    -source [get_pins apple_top_i/psram_driver_i/psram_clk_oddr_i/C] \
    -divide_by 1 \
    [get_ports psram_clk]

set Tce_sp 2.5 
set Tce_hd 2.5
set To_sp 2
set To_hd 2
set Ti_sp 9
set Ti_hd 6

# the board delays are tiny and very uniform, calling them all 10ps is good enough
set Tskew_min 0.01 
set Tskew_max 0.01

# ensure the psram_oe signals get IOB packed
# I believe the psram_i and psram_o signals get automatically inferred as IOB?
set_property IOB TRUE [get_cells -hierarchical {*psram_oe_reg*}];
set_property IOB TRUE [get_cells -hierarchical {*psram_ce_n_reg*}]
set_property IOB TRUE [get_cells -hierarchical {*psram_a_o_reg*}]
set_property IOB TRUE [get_cells -hierarchical {*psram_b_o_reg*}]
#set_property IOB TRUE [get_cells -hierarchical {*a_q_neg_reg*}]
#set_property IOB TRUE [get_cells -hierarchical {*b_q_neg_reg*}]

# psram io outputs have common setup/hold requirements
set_output_delay -clock psram_clk_out -max [expr $To_sp + $Tskew_max] [get_ports "psram_a_io[*]"]
set_output_delay -clock psram_clk_out -max [expr $To_sp + $Tskew_max] [get_ports "psram_b_io[*]"]
set_output_delay -clock psram_clk_out -min [expr -$To_hd + $Tskew_min] [get_ports "psram_a_io[*]"]
set_output_delay -clock psram_clk_out -min [expr -$To_hd + $Tskew_min] [get_ports "psram_b_io[*]"]

# set_input_delay -clock_fall -clock psram_clk_out -max [expr $Ti_sp + $Tskew_max] [get_ports "psram_a_io[*]"]
# set_input_delay -clock_fall -clock psram_clk_out -max [expr $Ti_sp + $Tskew_max] [get_ports "psram_b_io[*]"]
# set_input_delay -clock_fall -clock psram_clk_out -min [expr $Ti_hd + $Tskew_min] [get_ports "psram_a_io[*]"]
# set_input_delay -clock_fall -clock psram_clk_out -min [expr $Ti_hd + $Tskew_min] [get_ports "psram_b_io[*]"]

# set_multicycle_path 2 -setup -from [get_ports psram_a_io[*]] -to [get_cells -hier *u_a_iddr*]
# set_multicycle_path 2 -setup -from [get_ports psram_b_io[*]] -to [get_cells -hier *u_b_iddr*]
# set_multicycle_path 1 -hold -from [get_ports psram_a_io[*]] -to [get_cells -hier *u_a_iddr*]
# set_multicycle_path 1 -hold -from [get_ports psram_b_io[*]] -to [get_cells -hier *u_b_iddr*]

# dynamic timing on the inputs means we have to tell Vivado to ignore the input path
set_false_path -from [get_ports "psram_a_io[*]"]
set_false_path -from [get_ports "psram_b_io[*]"]

# ce_n has special output setup and hold requirements.
set_output_delay -clock psram_clk_out -max [expr $Tce_sp + $Tskew_max] [get_ports "psram_ce_n"]
set_output_delay -clock psram_clk_out -min [expr -$Tce_hd + $Tskew_min] [get_ports "psram_ce_n"]

################################################################################
# DVI Source-Synchronous Output Timing
################################################################################

## Generated clock on forwarded DVI clock output (ODDR in PL video subsystem).
## The PL forwards the clock half a pixel later than the internal SDR data
## launch, so model the output clock with source edges {2 3 4} rather than the
## default {1 2 3}. Use a NAME filter with -hierarchical because direct
## wildcard pin patterns do not reliably match hierarchical pin names across
## Vivado run stages.
create_generated_clock -name dvi_clk_out \
    -source [get_pins -hierarchical -filter {NAME =~ *video_top_i/dvi_clk_oddr/C}] \
    -edges {2 3 4} \
    [get_ports dvi_clk]

## TFP410PAP input timing (from datasheet):
##   Setup (tsu): 0.7 ns min
##   Hold  (th):  1.4 ns min
## A forwarded source-synchronous clock expresses the receiver hold requirement
## as a negative minimum output delay.
set_output_delay -clock dvi_clk_out -max 2.0 \
    [get_ports {dvi_red[*] dvi_grn[*] dvi_blu[*] dvi_de dvi_hsync dvi_vsync}]
set_output_delay -clock dvi_clk_out -min -1.4 \
    [get_ports {dvi_red[*] dvi_grn[*] dvi_blu[*] dvi_de dvi_hsync dvi_vsync}]

################################################################################
# I/O Timing Constraints
################################################################################

# ## Apple //e Bus Setup/Hold Times
# ## These are conservative estimates and should be verified
# set_input_delay -clock [get_clocks a2_phi0_clk] -min 10.000 [get_ports {a2fpga_a[*] a2fpga_d[*] a2fpga_rdwr_n a2fpga_devsel_n a2fpga_m2sel}]
# set_input_delay -clock [get_clocks a2_phi0_clk] -max 100.000 [get_ports {a2fpga_a[*] a2fpga_d[*] a2fpga_rdwr_n a2fpga_devsel_n a2fpga_m2sel}]

# set_output_delay -clock [get_clocks a2_phi0_clk] -min -5.000 [get_ports {a2fpga_a[*] a2fpga_d[*]}]
# set_output_delay -clock [get_clocks a2_phi0_clk] -max 20.000 [get_ports {a2fpga_a[*] a2fpga_d[*]}]

################################################################################
# Apple //e bus input-capture route bounds
################################################################################
#
# The bus pins are asynchronous and enter the design through 2-FF
# cdc_bus_sampled synchronizers (ASYNC_REG). Without a bound, the router is
# free to give every pad -> sync_meta capture route a different — and
# per-build different — delay. apple_bus_wrapper schedules its address/data
# sample taps relative to where it SEES the PHI0 edge through its own
# synchronizer, so the pin-to-pin route differential directly shifts the
# effective sample points within the 1 MHz bus cycle. The address snap tap
# (TAP_ADDR_SNAP) sits close to the 6502's address-transition window; an
# unlucky placement (short PHI0 route, long address routes) moves the snap
# into that window and the slot ROM gets served from mis-decoded addresses.
# Observed as: boot behavior changing with every resynthesis (no boot menu /
# crash to monitor) while all constrained timing reports stay clean.
#
# -datapath_only bounds the pad -> first-FF route without pretending these
# async pins are synchronous to the fabric clock. 5 ns is comfortable for
# an IBUF plus a short route and collapses the differential to ~2-3 ns.
set a2_bus_in_ports [get_ports {a2fpga_a[*] a2fpga_d[*] a2fpga_rdwr_n \
    a2fpga_clk a2fpga_m2sel a2fpga_m2b0 a2fpga_inh_n a2fpga_reset_n \
    a2fpga_irq_n a2fpga_rdy_n a2fpga_dma_n}]
set_max_delay -datapath_only 5.0 -from $a2_bus_in_ports

# The level-shifter direction outputs race the pad tristate enables at the
# board transceivers by design; bound them so that race margin is
# placement-independent as well. (Plain set_max_delay: -datapath_only is
# invalid without -from and Vivado drops the constraint. The oe_n pins are
# static pass-throughs of the 5V presence detect and need no bound.)
set_max_delay 10.0 -to [get_ports {a2fpga_dir_a a2fpga_dir_d}]

################################################################################
# Switching Activity (for power estimation)
################################################################################

## Apple //e bus data, address, and read/write: ~0.5 transitions/usec = 0.5 MHz
set_switching_activity -signal_rate 0.5 -static_probability 0.5 \
    [get_ports {a2fpga_d[*] a2fpga_a[*] a2fpga_rdwr_n}]

## Apple //e PHI0 clock: 1 transition/usec = 1 MHz
set_switching_activity -signal_rate 1.0 -static_probability 0.5 [get_ports a2fpga_clk]

## DVI pins: 0.5 transitions per 7 ns ≈ 71.43 MHz
set_switching_activity -signal_rate 71.43 -static_probability 0.5 \
    [get_ports {dvi_clk dvi_de dvi_hsync dvi_vsync \
                dvi_red[*] dvi_grn[*] dvi_blu[*]}]

## PSRAM pins: 133 MHz interface, but exercised at ~1% of max due to cache hit rate
## ~1.33 MHz effective signal rate
set_switching_activity -signal_rate 1.33 -static_probability 0.5 \
    [get_ports {psram_a_io[*] psram_b_io[*] psram_ce_n psram_clk}]

## Audio MCLK: 12.288 MHz DAC/audio oscillator
set_switching_activity -signal_rate 12.288 -static_probability 0.5 [get_ports audio_i2s_mclk]

## All other input/output pins default to a very low switching rate (1 kHz).
set_switching_activity -signal_rate 0.001 -static_probability 0.5 \
    [get_ports {a2ctrl_irq_n a2ctrl_nmi_n a2ctrl_rdy_n a2ctrl_dma_n \
                a2ctrl_inh_n a2ctrl_reset_n \
                a2fpga_dma_n a2fpga_rdy_n a2fpga_inh_n a2fpga_nmi_n \
                a2fpga_reset_n a2fpga_irq_n \
                a2fpga_oe_n a2fpga_oe_n_aux a2fpga_dir_a a2fpga_dir_d \
                a2fpga_7m a2fpga_q3 a2fpga_m2b0 a2fpga_m2sel a2fpga_devsel_n \
                dvi_scl dvi_sda dvi_hpd \
                audio_i2s_lrck audio_i2s_sdata audio_i2s_bick audio_spdif_out \
                i2c0_rtc_int_n i2c0_rtc_clkout i2c0_temp_alert_n \
                eth_d[*] eth_a[*] eth_rd_n eth_wr_n eth_cs_n \
                eth_rst_n eth_int_n usb_sys_fault_n}]

################################################################################
# CDC reset synchronizer false paths
################################################################################

## reset_sync (hdl/reset_sync.sv) implements async-assert / sync-deassert: the
## external arst_n drives the FDCE CLR pin of the synchronizer's sync_ff[0]
## flop, which by construction crosses clock domains. Vivado tries to time
## this as a Recovery check against the destination clock and fails (the
## requirement is sub-ns and the LUT1 inverter routing is non-trivial). The
## CDC pattern is correct — sync_ff has ASYNC_REG=TRUE — so this path is
## legitimately a false path.
##
## NAME filters expand square brackets as glob char classes; we resolve cells
## first (so the bracket survives literally) and then derive the CLR pin.
set_false_path -through \
    [get_pins -of_objects [get_cells -hierarchical -filter {NAME =~ *reset_sync_*/sync_ff_reg*}] \
              -filter {REF_PIN_NAME == CLR}]

################################################################################
# Configuration Settings
################################################################################

## Bitstream Configuration
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property CFGBVS VCCO [current_design]

################################################################################
# End of Constraints
################################################################################
