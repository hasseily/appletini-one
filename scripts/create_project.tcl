################################################################################
# Vivado Project Creation Script for Appletini One
#
# This script creates a complete Vivado project with:
#   - Zynq-7020 PS configured for all peripherals
#   - Pin constraints from CSV file
#   - Top-level wrapper and all active peripheral logic
#
# Usage:
#   vivado -mode batch -source scripts/create_project.tcl
#   or
#   vivado -mode tcl -source scripts/create_project.tcl
#
# Hardware Components (from AppleTini_BOM.txt):
#   - FPGA: XC7Z020-2CLG484I (Xilinx Zynq-7020, speed grade -2)
#   - DDR3L: K4B4G1646E-BYMA (Samsung, 2x 4Gbit = 1GB total)
#   - PSRAM: LY68L6400SLIT (Lyontek, 2x 64Mbit SPI/QPI = 16MB total)
#   - Flash: Production 128Mbit Quad SPI NOR = 16MB
#   - Ethernet: W5100S (WIZnet 10/100, parallel MCU interface)
#   - DVI: TFP410PAP (Texas Instruments DVI transmitter)
#   - USB PHY: USB3300-EZK-TR (Microchip, 2x ULPI)
#   - RTC: PCF8563DTR (I2C Real-Time Clock)
#   - Audio DAC: AK4493SEQ (Asahi Kasei 32-bit stereo DAC)
#   - USB-UART: CP2105-F01-GMR (Silicon Labs dual UART bridge)
#   - Temp Sensor: TMP102AIDRLR (Texas Instruments I2C)
#
################################################################################

# Script configuration
set project_name "appletini_yarz"
set project_dir "./project"
set part_name "xc7z020clg484-2"

# Get script directory for relative paths
set script_dir [file dirname [file normalize [info script]]]
set repo_root [file normalize [file join $script_dir ".."]]

puts "=========================================="
puts "Creating Appletini One Vivado Project"
puts "=========================================="
puts "Project: $project_name"
puts "Part: $part_name"
puts "Directory: $project_dir"
puts ""

# Create project directory if it doesn't exist
file mkdir $project_dir

# Create project
create_project $project_name $project_dir -part $part_name -force
set proj [current_project]

# Set project properties
set_property target_language Verilog $proj
set_property simulator_language Mixed $proj
set_property default_lib work $proj

puts "Project created successfully"
puts ""

################################################################################
# Add Source Files
################################################################################

puts "Adding source files..."

set hdl_sources_name [file join $repo_root "hdl" "hdl_sources.txt"]
set hdl_sources_handle [open $hdl_sources_name r]
set hdl_sources_data [read -nonewline $hdl_sources_handle]
close $hdl_sources_handle
set hdl_sources_list [split $hdl_sources_data "\n"]

foreach line $hdl_sources_list {
    set raw [string trim $line]
    if {$raw eq ""} {
        continue;
    }
    if {[string index $raw 0] eq "#"} {
        continue;
    }
    # Optional "path|library" syntax. The pipe-separated suffix is the
    # VHDL library to compile into (only meaningful for .vhd files); for
    # everything else it's just stripped.
    set lib_override ""
    set pipe_idx [string first "|" $raw]
    if {$pipe_idx >= 0} {
        set fn         [string trim [string range $raw 0 [expr {$pipe_idx - 1}]]]
        set lib_override [string trim [string range $raw [expr {$pipe_idx + 1}] end]]
    } else {
        set fn $raw
    }
    set file_suffix [file extension $fn]
    if {$file_suffix eq ".v"} {
        add_files -norecurse [file join $repo_root "hdl" $fn]
        set_property file_type Verilog [get_files [file tail $fn]]
    }
    if {$file_suffix eq ".sv"} {
        add_files -norecurse [file join $repo_root "hdl" $fn]
        set_property file_type SystemVerilog [get_files [file tail $fn]]
    }
    if {$file_suffix eq ".vhd"} {
        add_files -norecurse [file join $repo_root "hdl" $fn]
        set_property file_type VHDL [get_files [file tail $fn]]
        if {$lib_override ne ""} {
            set_property library $lib_override [get_files [file tail $fn]]
        }
    }
    if {$file_suffix eq ".mem"} {
        add_files -norecurse [file join $repo_root "hdl" $fn]
    }
    if {$fn eq "globals.sv"} {
        set_property is_global_include true [get_files [file tail $fn]]
    }
    if {$file_suffix eq ".xdc"} {
        add_files -fileset constrs_1 -norecurse [file join $repo_root "hdl" $fn]
    }
}

puts "Source files added"
puts ""

################################################################################
# Create Block Design
################################################################################

puts "Creating block design..."

create_bd_design "zynq_ps_bd"

################################################################################
# Add and Configure Zynq PS IP
################################################################################

puts "Adding and configuring Zynq PS..."

# Create Zynq PS IP
set zynq_ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 processing_system7_0]

# Apply board preset (basic configuration)
# Since this is a custom board, we'll configure manually
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 -config {
    make_external "FIXED_IO, DDR"
    apply_board_preset "0"
    Master "Disable"
    Slave "Disable"
} $zynq_ps

# Configure PS IP
set_property -dict [list \
	CONFIG.PCW_FPGA_FCLK0_ENABLE {1} \
	CONFIG.PCW_FPGA_FCLK1_ENABLE {0} \
    CONFIG.PCW_FPGA_FCLK2_ENABLE {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {133.000000} \
    CONFIG.PCW_FPGA2_PERIPHERAL_FREQMHZ {150.000000} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_EN_CLK1_PORT {0} \
    CONFIG.PCW_EN_CLK2_PORT {1} \
    CONFIG.PCW_FCLK0_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_FCLK2_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_FCLK_CLK0_BUF {TRUE} \
    CONFIG.PCW_FCLK_CLK2_BUF {TRUE} \
    CONFIG.PCW_EN_RST0_PORT {1} \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP1 {1} \
    CONFIG.PCW_USE_S_AXI_HP2 {1} \
    CONFIG.PCW_USE_S_AXI_HP3 {1} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
    CONFIG.PCW_IRQ_F2P_MODE {DIRECT} \
] $zynq_ps

# Clock Configuration
set_property -dict [list \
    CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ {33.333333} \
    CONFIG.PCW_APU_CLK_RATIO_ENABLE {6:2:1} \
    CONFIG.PCW_CPU_PERIPHERAL_CLKSRC {ARM PLL} \
    CONFIG.PCW_DDR_PERIPHERAL_CLKSRC {DDR PLL} \
    CONFIG.PCW_QSPI_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_SDIO_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_SPI_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_UART_PERIPHERAL_CLKSRC {IO PLL} \
    CONFIG.PCW_SPI_PERIPHERAL_FREQMHZ {166.666666} \
] $zynq_ps

# DDR Configuration (32-bit DDR3L)
# Hardware: 2x Samsung K4B4G1646E-BYMA (U122, U222)
# - 4Gbit (512MB) per chip, 16-bit width each = 1GB total
# - DDR3L-1866 (933MHz) @ 1.35V
# Using MT41K256M16 RE-125 as close equivalent (DDR3L-1600, similar to K4B4G1646E)
# Default/stable bin. Bump to DDR3L_1066 only for explicit OC experiments.
set ddr_speed_bin "DDR3L_800"
set_property -dict [list \
    CONFIG.PCW_UIPARAM_DDR_PARTNO {MT41K256M16 RE-125} \
    CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit} \
    CONFIG.PCW_UIPARAM_DDR_DEVICE_CAPACITY {4096 MBits} \
    CONFIG.PCW_UIPARAM_DDR_SPEED_BIN $ddr_speed_bin \
    CONFIG.PCW_UIPARAM_DDR_TRAIN_WRITE_LEVEL {1} \
    CONFIG.PCW_UIPARAM_DDR_TRAIN_READ_GATE {1} \
    CONFIG.PCW_UIPARAM_DDR_TRAIN_DATA_EYE {1} \
    CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_0 {0.048} \
    CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_1 {0.050} \
    CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_2 {0.048} \
    CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_3 {0.050} \
    CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY0 {0.294} \
    CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY1 {0.298} \
    CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY2 {0.294} \
    CONFIG.PCW_UIPARAM_DDR_BOARD_DELAY3 {0.298} \
] $zynq_ps

# MIO Configuration - Bank 500 (MIO 0-15)
# QSPI: MIO 1-6 (Quad SPI for production NOR flash)
# Simplified config for Vivado 2025.2 - group settings are auto-configured
set_property -dict [list \
    CONFIG.PCW_QSPI_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_QSPI_QSPI_IO {MIO 1 .. 6} \
    CONFIG.PCW_QSPI_GRP_FBCLK_ENABLE {0} \
] $zynq_ps

# MIO Pin Configuration for QSPI (MIO 1-6)
# Set fast slew rate for high-speed operation (133 MHz QSPI clock)
# All QSPI pins have external pullups - disable internal pullups to avoid conflicts
set_property -dict [list \
    CONFIG.PCW_MIO_1_SLEW {fast} \
    CONFIG.PCW_MIO_1_PULLUP {disabled} \
    CONFIG.PCW_MIO_2_SLEW {fast} \
    CONFIG.PCW_MIO_2_PULLUP {disabled} \
    CONFIG.PCW_MIO_3_SLEW {fast} \
    CONFIG.PCW_MIO_3_PULLUP {disabled} \
    CONFIG.PCW_MIO_4_SLEW {fast} \
    CONFIG.PCW_MIO_4_PULLUP {disabled} \
    CONFIG.PCW_MIO_5_SLEW {fast} \
    CONFIG.PCW_MIO_5_PULLUP {disabled} \
    CONFIG.PCW_MIO_6_SLEW {fast} \
    CONFIG.PCW_MIO_6_PULLUP {disabled} \
] $zynq_ps

# SDIO1: MIO 10-15 (SD card), CD: MIO 9
# Force fast slew on the SD pins so 50 MHz high-speed edges have clean
# rise/fall times. MIO defaults to slow slew which causes setup/hold
# violations and forces the controller back to default (25 MHz) speed
# on marginal cards.
set_property -dict [list \
    CONFIG.PCW_SD1_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_SD1_SD1_IO {MIO 10 .. 15} \
    CONFIG.PCW_SD1_GRP_CD_ENABLE {1} \
    CONFIG.PCW_SD1_GRP_CD_IO {MIO 9} \
    CONFIG.PCW_SD1_GRP_WP_ENABLE {0} \
    CONFIG.PCW_MIO_9_SLEW {fast} \
    CONFIG.PCW_MIO_10_SLEW {fast} \
    CONFIG.PCW_MIO_11_SLEW {fast} \
    CONFIG.PCW_MIO_12_SLEW {fast} \
    CONFIG.PCW_MIO_13_SLEW {fast} \
    CONFIG.PCW_MIO_14_SLEW {fast} \
    CONFIG.PCW_MIO_15_SLEW {fast} \
] $zynq_ps

# Ethernet uses the PL-facing W5100S parallel MCU interface. Keep both PS GEM
# blocks disabled so exported platforms do not expose unused XEMACPS devices.
set_property -dict [list \
    CONFIG.PCW_EN_ENET0 {0} \
    CONFIG.PCW_EN_ENET1 {0} \
    CONFIG.PCW_ENET0_PERIPHERAL_ENABLE {0} \
    CONFIG.PCW_ENET1_PERIPHERAL_ENABLE {0} \
    CONFIG.PCW_ENET0_GRP_MDIO_ENABLE {0} \
    CONFIG.PCW_ENET1_GRP_MDIO_ENABLE {0} \
] $zynq_ps

# I2C0: MIO 22-23
set_property -dict [list \
    CONFIG.PCW_I2C0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_I2C0_I2C0_IO {MIO 22 .. 23} \
    CONFIG.PCW_I2C0_GRP_INT_ENABLE {0} \
] $zynq_ps

# UART0: MIO 26 (RX), 27 (TX) - pins must be specified lower..higher
set_property -dict [list \
    CONFIG.PCW_UART0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_UART0_UART0_IO {MIO 26 .. 27} \
    CONFIG.PCW_UART0_GRP_FULL_ENABLE {0} \
    CONFIG.PCW_UART0_BAUD_RATE {115200} \
] $zynq_ps

# UART1: MIO 24 (TX), 25 (RX) - pins must be specified lower..higher
set_property -dict [list \
    CONFIG.PCW_UART1_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_UART1_UART1_IO {MIO 24 .. 25} \
    CONFIG.PCW_UART1_GRP_FULL_ENABLE {0} \
    CONFIG.PCW_UART1_BAUD_RATE {115200} \
] $zynq_ps

# USB0: MIO 28-39 (ULPI)
set_property -dict [list \
    CONFIG.PCW_USB0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_USB0_USB0_IO {MIO 28 .. 39} \
    CONFIG.PCW_USB0_RESET_ENABLE {0} \
] $zynq_ps

# USB1: MIO 40-51 (ULPI)
set_property -dict [list \
    CONFIG.PCW_USB1_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_USB1_USB1_IO {MIO 40 .. 51} \
    CONFIG.PCW_USB1_RESET_ENABLE {0} \
] $zynq_ps

# USB ULPI MIO pin electrical settings: force FAST slew on MIO 28-51
set_property -dict [list \
    CONFIG.PCW_MIO_28_SLEW {fast} \
    CONFIG.PCW_MIO_29_SLEW {fast} \
    CONFIG.PCW_MIO_30_SLEW {fast} \
    CONFIG.PCW_MIO_31_SLEW {fast} \
    CONFIG.PCW_MIO_32_SLEW {fast} \
    CONFIG.PCW_MIO_33_SLEW {fast} \
    CONFIG.PCW_MIO_34_SLEW {fast} \
    CONFIG.PCW_MIO_35_SLEW {fast} \
    CONFIG.PCW_MIO_36_SLEW {fast} \
    CONFIG.PCW_MIO_37_SLEW {fast} \
    CONFIG.PCW_MIO_38_SLEW {fast} \
    CONFIG.PCW_MIO_39_SLEW {fast} \
    CONFIG.PCW_MIO_40_SLEW {fast} \
    CONFIG.PCW_MIO_41_SLEW {fast} \
    CONFIG.PCW_MIO_42_SLEW {fast} \
    CONFIG.PCW_MIO_43_SLEW {fast} \
    CONFIG.PCW_MIO_44_SLEW {fast} \
    CONFIG.PCW_MIO_45_SLEW {fast} \
    CONFIG.PCW_MIO_46_SLEW {fast} \
    CONFIG.PCW_MIO_47_SLEW {fast} \
    CONFIG.PCW_MIO_48_SLEW {fast} \
    CONFIG.PCW_MIO_49_SLEW {fast} \
    CONFIG.PCW_MIO_50_SLEW {fast} \
    CONFIG.PCW_MIO_51_SLEW {fast} \
] $zynq_ps

# GPIO - Enable EMIO for additional GPIOs if needed
set_property -dict [list \
    CONFIG.PCW_GPIO_MIO_GPIO_ENABLE {1} \
    CONFIG.PCW_GPIO_EMIO_GPIO_ENABLE {0} \
] $zynq_ps

# Boot Configuration
set_property -dict [list \
    CONFIG.PCW_UIPARAM_GENERATE_SUMMARY {1} \
] $zynq_ps

puts "Zynq PS configured"
puts ""

################################################################################
# Make Ports External
################################################################################

puts "Creating external ports..."

set M_AXI_GP0_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_GP0_0 ]
set_property -dict [ list \
    CONFIG.ADDR_WIDTH {32} \
    CONFIG.DATA_WIDTH {32} \
    CONFIG.HAS_REGION {0} \
    CONFIG.NUM_READ_OUTSTANDING {8} \
    CONFIG.NUM_WRITE_OUTSTANDING {8} \
    CONFIG.PROTOCOL {AXI3} \
] $M_AXI_GP0_0


# set M01_AXI_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M01_AXI_0 ] 
# set_property -dict [ list \
#     CONFIG.ADDR_WIDTH {32} \
#     CONFIG.DATA_WIDTH {32} \
#     CONFIG.PROTOCOL {AXI4LITE} \
# ] $M01_AXI_0

# set M02_AXI_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M02_AXI_0 ]
# set_property -dict [ list \
#     CONFIG.ADDR_WIDTH {32} \
#     CONFIG.DATA_WIDTH {32} \
#     CONFIG.PROTOCOL {AXI4LITE} \
# ] $M02_AXI_0

# set M03_AXI_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M03_AXI_0 ]
# set_property -dict [ list \
#     CONFIG.ADDR_WIDTH {32} \
#     CONFIG.DATA_WIDTH {32} \
#     CONFIG.PROTOCOL {AXI4LITE} \
# ] $M03_AXI_0

# create AXI3 interface for video
set S_AXI_HP0_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_HP0_0 ]
set_property -dict [ list \
    CONFIG.ADDR_WIDTH {32} \
    CONFIG.ARUSER_WIDTH {0} \
    CONFIG.AWUSER_WIDTH {0} \
    CONFIG.BUSER_WIDTH {0} \
    CONFIG.DATA_WIDTH {64} \
    CONFIG.HAS_BRESP {1} \
    CONFIG.HAS_BURST {1} \
    CONFIG.HAS_CACHE {1} \
    CONFIG.HAS_LOCK {1} \
    CONFIG.HAS_PROT {1} \
    CONFIG.HAS_QOS {1} \
    CONFIG.HAS_REGION {0} \
    CONFIG.HAS_RRESP {1} \
    CONFIG.HAS_WSTRB {1} \
    CONFIG.ID_WIDTH {0} \
    CONFIG.MAX_BURST_LENGTH {16} \
    CONFIG.NUM_READ_OUTSTANDING {8} \
    CONFIG.NUM_READ_THREADS {1} \
    CONFIG.NUM_WRITE_OUTSTANDING {8} \
    CONFIG.NUM_WRITE_THREADS {1} \
    CONFIG.PROTOCOL {AXI3} \
    CONFIG.READ_WRITE_MODE {READ_WRITE} \
    CONFIG.RUSER_BITS_PER_BYTE {0} \
    CONFIG.RUSER_WIDTH {0} \
    CONFIG.SUPPORTS_NARROW_BURST {1} \
    CONFIG.WUSER_BITS_PER_BYTE {0} \
    CONFIG.WUSER_WIDTH {0} \
] $S_AXI_HP0_0

# create AXI3 interface for 133MHz utility
set S_AXI_HP1_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_HP1_0 ]
set_property -dict [ list \
    CONFIG.ADDR_WIDTH {32} \
    CONFIG.ARUSER_WIDTH {0} \
    CONFIG.AWUSER_WIDTH {0} \
    CONFIG.BUSER_WIDTH {0} \
    CONFIG.DATA_WIDTH {64} \
    CONFIG.HAS_BRESP {1} \
    CONFIG.HAS_BURST {1} \
    CONFIG.HAS_CACHE {0} \
    CONFIG.HAS_LOCK {0} \
    CONFIG.HAS_PROT {0} \
    CONFIG.HAS_QOS {0} \
    CONFIG.HAS_REGION {0} \
    CONFIG.HAS_RRESP {1} \
    CONFIG.HAS_WSTRB {1} \
    CONFIG.ID_WIDTH {0} \
    CONFIG.MAX_BURST_LENGTH {16} \
    CONFIG.NUM_READ_OUTSTANDING {8} \
    CONFIG.NUM_READ_THREADS {1} \
    CONFIG.NUM_WRITE_OUTSTANDING {8} \
    CONFIG.NUM_WRITE_THREADS {1} \
    CONFIG.PROTOCOL {AXI3} \
    CONFIG.READ_WRITE_MODE {READ_WRITE} \
    CONFIG.RUSER_BITS_PER_BYTE {0} \
    CONFIG.RUSER_WIDTH {0} \
    CONFIG.SUPPORTS_NARROW_BURST {1} \
    CONFIG.WUSER_BITS_PER_BYTE {0} \
    CONFIG.WUSER_WIDTH {0} \
] $S_AXI_HP1_0

# create AXI3 interface for dedicated Disk II audio sample reads
set S_AXI_HP2_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_HP2_0 ]
set_property -dict [ list \
    CONFIG.ADDR_WIDTH {32} \
    CONFIG.ARUSER_WIDTH {0} \
    CONFIG.AWUSER_WIDTH {0} \
    CONFIG.BUSER_WIDTH {0} \
    CONFIG.DATA_WIDTH {64} \
    CONFIG.HAS_BRESP {1} \
    CONFIG.HAS_BURST {1} \
    CONFIG.HAS_CACHE {0} \
    CONFIG.HAS_LOCK {0} \
    CONFIG.HAS_PROT {0} \
    CONFIG.HAS_QOS {0} \
    CONFIG.HAS_REGION {0} \
    CONFIG.HAS_RRESP {1} \
    CONFIG.HAS_WSTRB {1} \
    CONFIG.ID_WIDTH {0} \
    CONFIG.MAX_BURST_LENGTH {16} \
    CONFIG.NUM_READ_OUTSTANDING {8} \
    CONFIG.NUM_READ_THREADS {1} \
    CONFIG.NUM_WRITE_OUTSTANDING {8} \
    CONFIG.NUM_WRITE_THREADS {1} \
    CONFIG.PROTOCOL {AXI3} \
    CONFIG.READ_WRITE_MODE {READ_WRITE} \
    CONFIG.RUSER_BITS_PER_BYTE {0} \
    CONFIG.RUSER_WIDTH {0} \
    CONFIG.SUPPORTS_NARROW_BURST {1} \
    CONFIG.WUSER_BITS_PER_BYTE {0} \
    CONFIG.WUSER_WIDTH {0} \
] $S_AXI_HP2_0

set S_AXI_HP3_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_HP3_0 ]
set_property -dict [ list \
    CONFIG.ADDR_WIDTH {32} \
    CONFIG.ARUSER_WIDTH {0} \
    CONFIG.AWUSER_WIDTH {0} \
    CONFIG.BUSER_WIDTH {0} \
    CONFIG.DATA_WIDTH {64} \
    CONFIG.HAS_BRESP {1} \
    CONFIG.HAS_BURST {1} \
    CONFIG.HAS_CACHE {0} \
    CONFIG.HAS_LOCK {0} \
    CONFIG.HAS_PROT {0} \
    CONFIG.HAS_QOS {0} \
    CONFIG.HAS_REGION {0} \
    CONFIG.HAS_RRESP {1} \
    CONFIG.HAS_WSTRB {1} \
    CONFIG.ID_WIDTH {0} \
    CONFIG.MAX_BURST_LENGTH {16} \
    CONFIG.NUM_READ_OUTSTANDING {8} \
    CONFIG.NUM_READ_THREADS {1} \
    CONFIG.NUM_WRITE_OUTSTANDING {8} \
    CONFIG.NUM_WRITE_THREADS {1} \
    CONFIG.PROTOCOL {AXI3} \
    CONFIG.READ_WRITE_MODE {READ_WRITE} \
    CONFIG.RUSER_BITS_PER_BYTE {0} \
    CONFIG.RUSER_WIDTH {0} \
    CONFIG.SUPPORTS_NARROW_BURST {1} \
    CONFIG.WUSER_BITS_PER_BYTE {0} \
    CONFIG.WUSER_WIDTH {0} \
] $S_AXI_HP3_0

# Create reset and clock ports
# peripheral_133M_aresetn is widened to [7:0]: proc_sys_reset_0 produces 8
# independent driver flops (one per bit) which Vivado can place near each
# load cluster. Bit assignments (see appletini_yarz_top.sv and apple_top.sv):
#   [3:0] apple_top_i internal sub-partitions:
#       [0] memory subsystem (psram_simple, psram_driver, dma_engine,
#           cycle_capture, cycle_egress)
#       [1] apple bus path (bus_wrapper, soft_switch_manager,
#           timing_gen, write_arbiter)
#       [2] card emulation (mockingboard, smartport, boot_menu, no_slot_clock)
#       [3] axisimple shim + apple_top control-register block (ps_dma_command,
#           card_feature_enable, reset_release_ready, etc.)
#   [4] axisimple_wrapper at top + 16x irq_pulse_stretcher
#   [5] video_top_i
#   [6] audio path (audio_spdif_pcm_tx, fclk-domain audio acc, mockingboard CDC,
#       menu_chime CDC, audio_mclk reset sync)
#   [7] IDELAYCTRL (remaining bit is spare)
set peripheral_133M_aresetn [ create_bd_port -dir O -from 7 -to 0 -type rst peripheral_133M_aresetn ]
#set peripheral_150M_aresetn [ create_bd_port -dir O -from 0 -to 0 -type rst peripheral_150M_aresetn ]
set FCLK_CLK0 [ create_bd_port -dir O -type clk FCLK_CLK0 ]
set_property -dict [ list \
    CONFIG.ASSOCIATED_BUSIF {M_AXI_GP0_0:S_AXI_HP3_0:S_AXI_HP2_0:S_AXI_HP1_0:S_AXI_HP0_0} \
    CONFIG.ASSOCIATED_RESET {peripheral_133M_aresetn} \
] $FCLK_CLK0
set FCLK_CLK1 [ create_bd_port -dir O -type clk FCLK_CLK1 ]

set IRQ [ create_bd_port -dir I -from 15 -to 0 IRQ ]

# clock wizard generated 148.5 MHz clock and reset
set peripheral_148M_aresetn [ create_bd_port -dir O -from 0 -to 0 -type rst peripheral_148M_aresetn ]
set clk_pixel_148mhz [ create_bd_port -dir O -type clk clk_pixel_148mhz ]
set_property -dict [ list \
    CONFIG.ASSOCIATED_RESET {peripheral_148M_aresetn} \
] $clk_pixel_148mhz
# Create instance: proc_sys_reset_0, and set properties.
# C_NUM_PERP_ARESETN=8 produces 8 independent driver flops on
# peripheral_aresetn[7:0]. Each bit is functionally identical (same source
# state machine, same deassert cycle), but the placer can put each driver
# near its load cluster and avoid a cross-die reset broadcast.
set proc_sys_reset_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_0 ]
set_property -dict [list CONFIG.C_NUM_PERP_ARESETN {8}] $proc_sys_reset_0

# # Create instance: proc_sys_reset_1, and set properties
# set proc_sys_reset_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_1 ]

# Create instance: clk_wiz_0, and set properties
set clk_wiz_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 clk_wiz_0 ]
set_property -dict [list \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {148.5} \
    CONFIG.PRIM_IN_FREQ {150.000} \
    CONFIG.PRIM_SOURCE {No_buffer} \
    CONFIG.RESET_PORT {resetn} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
] $clk_wiz_0

# Create instance: proc_sys_reset_2, and set properties
set proc_sys_reset_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_2 ]

# Create instance: clk_wiz_1, and set properties
set clk_wiz_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 clk_wiz_1 ]
    set_property -dict [list \
    #CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {100.0000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {133.33300} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {199.9950} \
    CONFIG.CLKOUT2_USED {true} \
    CONFIG.NUM_OUT_CLKS {2} \
    CONFIG.PRIM_SOURCE {No_buffer} \
    CONFIG.RESET_PORT {resetn} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
    ] $clk_wiz_1

# Create instance: ilconcat_0, and set properties
set ilconcat_0 [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconcat:1.0 ilconcat_0 ]
set_property -dict [list \
    CONFIG.IN0_WIDTH {16} \
    CONFIG.NUM_PORTS {1} \
] $ilconcat_0

# Create interface connections
connect_bd_intf_net -intf_net S_AXI_HP0_0_1 [get_bd_intf_ports S_AXI_HP0_0] [get_bd_intf_pins processing_system7_0/S_AXI_HP0]
connect_bd_intf_net -intf_net S_AXI_HP1_0_1 [get_bd_intf_ports S_AXI_HP1_0] [get_bd_intf_pins processing_system7_0/S_AXI_HP1]
connect_bd_intf_net -intf_net S_AXI_HP2_0_1 [get_bd_intf_ports S_AXI_HP2_0] [get_bd_intf_pins processing_system7_0/S_AXI_HP2]
connect_bd_intf_net -intf_net S_AXI_HP3_0_1 [get_bd_intf_ports S_AXI_HP3_0] [get_bd_intf_pins processing_system7_0/S_AXI_HP3]
connect_bd_intf_net -intf_net M_AXI_GP0_0_1 [get_bd_intf_ports M_AXI_GP0_0] [get_bd_intf_pins processing_system7_0/M_AXI_GP0]

  # Create port connections
  connect_bd_net -net clk_wiz_0_clk_out1  [get_bd_pins clk_wiz_0/clk_out1] \
  [get_bd_pins proc_sys_reset_2/slowest_sync_clk] \
  [get_bd_ports clk_pixel_148mhz]
  connect_bd_net -net clk_wiz_0_locked  [get_bd_pins clk_wiz_0/locked] \
  [get_bd_pins proc_sys_reset_2/dcm_locked]
  connect_bd_net -net clk_wiz_1_clk_out1  [get_bd_pins clk_wiz_1/clk_out1] \
  [get_bd_pins proc_sys_reset_0/slowest_sync_clk] \
  [get_bd_ports FCLK_CLK0] \
  [get_bd_pins processing_system7_0/M_AXI_GP0_ACLK] \
  [get_bd_pins processing_system7_0/S_AXI_HP0_ACLK] \
  [get_bd_pins processing_system7_0/S_AXI_HP1_ACLK] \
  [get_bd_pins processing_system7_0/S_AXI_HP2_ACLK] \
  [get_bd_pins processing_system7_0/S_AXI_HP3_ACLK]
  connect_bd_net -net clk_wiz_1_clk_out2  [get_bd_pins clk_wiz_1/clk_out2] \
  [get_bd_ports FCLK_CLK1]
  connect_bd_net -net clk_wiz_1_locked  [get_bd_pins clk_wiz_1/locked] \
  [get_bd_pins proc_sys_reset_0/dcm_locked]
  connect_bd_net -net proc_sys_reset_0_peripheral_aresetn  [get_bd_pins proc_sys_reset_0/peripheral_aresetn] \
  [get_bd_ports peripheral_133M_aresetn]
  connect_bd_net -net proc_sys_reset_2_peripheral_aresetn  [get_bd_pins proc_sys_reset_2/peripheral_aresetn] \
  [get_bd_ports peripheral_148M_aresetn]
  connect_bd_net -net processing_system7_0_FCLK_CLK0  [get_bd_pins processing_system7_0/FCLK_CLK0] \
  [get_bd_pins clk_wiz_1/clk_in1]
  connect_bd_net -net processing_system7_0_FCLK_CLK2  [get_bd_pins processing_system7_0/FCLK_CLK2] \
  [get_bd_pins clk_wiz_0/clk_in1]
  connect_bd_net -net processing_system7_0_FCLK_RESET0_N  [get_bd_pins processing_system7_0/FCLK_RESET0_N] \
  [get_bd_pins proc_sys_reset_0/ext_reset_in] \
  [get_bd_pins proc_sys_reset_2/ext_reset_in] \
  [get_bd_pins clk_wiz_0/resetn] \
  [get_bd_pins clk_wiz_1/resetn]
  connect_bd_net -net IRQ_1  [get_bd_ports IRQ] \
  [get_bd_pins ilconcat_0/In0]
  connect_bd_net -net ilconcat_0_dout  [get_bd_pins ilconcat_0/dout] \
  [get_bd_pins processing_system7_0/IRQ_F2P]



puts "External ports created"
puts ""

################################################################################
# Address Editor Assignments
################################################################################

puts "Assigning address segments..."

assign_bd_address -offset 0x40000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces processing_system7_0/Data] [get_bd_addr_segs M_AXI_GP0_0/Reg] -force
assign_bd_address -offset 0x00000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces S_AXI_HP0_0] [get_bd_addr_segs processing_system7_0/S_AXI_HP0/HP0_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces S_AXI_HP1_0] [get_bd_addr_segs processing_system7_0/S_AXI_HP1/HP1_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces S_AXI_HP2_0] [get_bd_addr_segs processing_system7_0/S_AXI_HP2/HP2_DDR_LOWOCM] -force
assign_bd_address -offset 0x00000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces S_AXI_HP3_0] [get_bd_addr_segs processing_system7_0/S_AXI_HP3/HP3_DDR_LOWOCM] -force

puts "Address segments assigned"
puts ""

################################################################################
# Validate and Save Block Design
################################################################################

puts "Validating block design..."

# Regenerate layout
regenerate_bd_layout

# Validate design
validate_bd_design

save_bd_design

#set bd_files [get_files -quiet *zynq_ps_bd.bd]
#if {[llength $bd_files] > 0} {
#    if {[catch {set_property synth_checkpoint_mode None $bd_files} bd_checkpoint_error]} {
#        puts "WARNING: Could not disable block-design OOC checkpoints: $bd_checkpoint_error"
#    } else {
#        save_bd_design
#        puts "Block-design OOC checkpoints disabled"
#   }
#} else {
#    puts "WARNING: zynq_ps_bd.bd not found; block-design OOC checkpoints unchanged"
#}

puts "Block design validated and saved"
puts ""

################################################################################
# Generate HDL Wrapper
################################################################################

puts "Generating HDL wrapper..."

# Make wrapper file for block design
set wrapper_file [make_wrapper -files [get_files zynq_ps_bd.bd] -top]
add_files -norecurse $wrapper_file

# Set top module
set_property top appletini_yarz_top [current_fileset]

puts "HDL wrapper generated"
puts ""

################################################################################
# Set Synthesis and Implementation Strategies
################################################################################

puts "Configuring synthesis and implementation strategies..."

# Set synthesis strategy
set_property strategy "Vivado Synthesis Defaults" [get_runs synth_1]

# Set implementation strategy
set_property strategy "Vivado Implementation Defaults" [get_runs impl_1]
set_property steps.phys_opt_design.is_enabled true [get_runs impl_1]

puts "Strategies configured"
puts ""

################################################################################
# Project Creation Complete
################################################################################

puts "=========================================="
puts "Project creation complete!"
puts "=========================================="
puts "Project: project/appletini_yarz.xpr"
puts "Build/export: vivado -mode batch -source scripts/build_and_export_xsa.tcl"
