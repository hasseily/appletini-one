////////////////////////////////////////////////////////////////////////////////
// Appletini One Top-Level Module
//
// Apple //e Expansion Card - Multi-Card Emulator
// Based on Xilinx Zynq-7020 SoC FPGA
//
// This top-level module instantiates the Zynq PS and connects all PL IOs.
// The PS handles:
//   - DDR3 memory controller
//   - USB host interfaces (USB0/1)
//   - UART interfaces (UART0/1)
//   - SPI (to Ethernet controller)
//   - I2C (to RTC, temp sensor, etc.)
//   - QSPI flash boot and storage
//   - SD card interface
//
// The PL provides:
//   - Apple //e bus interface logic
//   - DVI video generation (test pattern or DDR framebuffer)
//   - Audio synthesis (I2S, SPDIF)
//   - PSRAM buffering
//   - Card emulation logic
////////////////////////////////////////////////////////////////////////////////

`timescale 1ns / 1ps

module appletini_yarz_top (
    //==========================================================================
    // Zynq PS Fixed IO and DDR Interface
    //==========================================================================
    inout  wire [53:0]  MIO,
    inout  wire         PS_CLK,
    inout  wire         PS_PORB,
    inout  wire         PS_SRSTB,
    inout  wire         DDR_VRP,
    inout  wire         DDR_VRN,
    inout  wire [14:0]  DDR_Addr,
    inout  wire [2:0]   DDR_BankAddr,
    inout  wire         DDR_CAS_n,
    inout  wire         DDR_Clk,
    inout  wire         DDR_Clk_n,
    inout  wire         DDR_CKE,
    inout  wire         DDR_CS_n,
    inout  wire [3:0]   DDR_DM,
    inout  wire [31:0]  DDR_DQ,
    inout  wire [3:0]   DDR_DQS,
    inout  wire [3:0]   DDR_DQS_n,
    inout  wire         DDR_ODT,
    inout  wire         DDR_RAS_n,
    inout  wire         DDR_DRSTB,
    inout  wire         DDR_WEB,

    //==========================================================================
    // Apple //e Bus Interface - Bank 33
    //==========================================================================

    // Control Signals - Assert (FPGA drives these open-collector)
    output logic        a2ctrl_irq_n,
    output logic        a2ctrl_nmi_n,
    output logic        a2ctrl_rdy_n,
    output logic        a2ctrl_dma_n,
    output logic        a2ctrl_inh_n,
    output logic        a2ctrl_reset_n,

    // Control Signals - Observe (FPGA reads these from bus)
    inout  logic        a2fpga_dma_n,
    inout  logic        a2fpga_rdy_n,
    inout  logic        a2fpga_inh_n,
    inout  logic        a2fpga_nmi_n,
    inout  logic        a2fpga_reset_n,
    inout  logic        a2fpga_irq_n,

    // Bus Transceiver Control
    output logic        a2fpga_oe_n,          // Main transceiver output enable
    output logic        a2fpga_oe_n_aux,      // Auxiliary transceiver output enable
    output logic        a2fpga_dir_a,         // Address bus direction control
    output logic        a2fpga_dir_d,         // Data bus direction control

    // Address Bus (bidirectional via level translators)
    inout  wire [15:0]  a2fpga_a,

    // Data Bus (bidirectional via level translators)
    inout  wire [7:0]   a2fpga_d,

    // Bus Control
    inout  wire         a2fpga_rdwr_n,        // Read/Write# signal

    // Clock Inputs from Apple //e
    input  logic        a2fpga_clk,           // PHI0 clock (~1.023 MHz)
    input  logic        a2fpga_7m,            // 7M clock (~7.159 MHz)
    input  logic        a2fpga_q3,            // Q3 clock (~2.046 MHz)

    // Slot Select and Status
    input  logic        a2fpga_m2b0,          // M2B0 status
    input  logic        a2fpga_m2sel,         // M2SEL slot select
    input  logic        a2fpga_devsel_n,      // DEVSEL# slot decode

    //==========================================================================
    // DVI Video Output - Bank 34
    //==========================================================================

    // Parallel RGB to TFP410
    output logic [4:0]  dvi_red,
    output logic [5:0]  dvi_grn,
    output logic [4:0]  dvi_blu,

    // Video Timing and Control
    output logic        dvi_clk,              // Pixel clock
    output logic        dvi_de,               // Data enable
    output logic        dvi_hsync,            // Horizontal sync
    output logic        dvi_vsync,            // Vertical sync

    // DVI I2C (DDC/EDID)
    inout  wire         dvi_scl,
    inout  wire         dvi_sda,

    // DVI Hot Plug Detect
    input  logic        dvi_hpd,

    //==========================================================================
    // Audio Output - Bank 34
    //==========================================================================

    // I2S Audio Interface
    input  logic        audio_i2s_mclk,       // Master clock input from DAC clock source
    output logic        audio_i2s_lrck,       // Word select (left/right)
    output logic        audio_i2s_sdata,      // Serial data
    output logic        audio_i2s_bick,       // Bit clock

    // SPDIF Digital Audio
    output logic        audio_spdif_out,

    //==========================================================================
    // PSRAM Interface - Bank 34
    //==========================================================================

    // PSRAM Chip A (4-bit data)
    inout  wire [3:0]   psram_a_io,

    // PSRAM Chip B (4-bit data)
    inout  wire [3:0]   psram_b_io,

    // PSRAM Control (shared between A and B)
    output logic        psram_ce_n,           // Chip enable (active low)
    output logic        psram_clk,            // Clock

    //==========================================================================
    // Peripheral Interfaces - Bank 34
    //==========================================================================

    // RTC (Real-Time Clock) on I2C0
    input  logic        i2c0_rtc_int_n,       // RTC interrupt
    input  logic        i2c0_rtc_clkout,      // RTC clock output

    // Temperature Sensor on I2C0
    input  logic        i2c0_temp_alert_n,    // Temperature alert

    //==========================================================================
    // Ethernet Controller Parallel Interface - Bank 35
    //==========================================================================

    inout  wire [7:0]   eth_d,                // W5100S data bus
    output logic [1:0]  eth_a,                // W5100S A0/A1
    output logic        eth_rd_n,             // W5100S RD#
    output logic        eth_wr_n,             // W5100S WR#
    output logic        eth_cs_n,             // W5100S CS#
    output logic        eth_rst_n,            // W5100S RST#
    input  logic        eth_int_n,            // W5100S INT#

    // USB System
    input  logic        usb_sys_fault_n       // USB power fault indicator
);

    //==========================================================================
    // Internal Signals
    //==========================================================================
    // Clock/reset domains used in this top:
    // - AXI/control: fclk_clk0 / peripheral_133M_aresetn
    // - IDELAY reference: fclk_clk1 (~200 MHz from clk_wiz_1 clk_out2)
    // - Video pixel: clk_pixel_148mhz / peripheral_148M_aresetn
    //   (from clk_wiz_0 driven by PS FCLK2)
    // - Audio MCLK: audio_i2s_mclk from the board audio oscillator on M19

    // AXI Interfaces from PS to PL
    // GP0 - General Purpose Master 0 (AXI3, 32-bit)
    wire [31:0]M_AXI_GP0_0_araddr;
    wire [1:0]M_AXI_GP0_0_arburst;
    wire [3:0]M_AXI_GP0_0_arcache;
    wire [11:0]M_AXI_GP0_0_arid;
    wire [3:0]M_AXI_GP0_0_arlen;
    wire [1:0]M_AXI_GP0_0_arlock;
    wire [2:0]M_AXI_GP0_0_arprot;
    wire [3:0]M_AXI_GP0_0_arqos;
    wire M_AXI_GP0_0_arready;
    wire [2:0]M_AXI_GP0_0_arsize;
    wire M_AXI_GP0_0_arvalid;
    wire [31:0]M_AXI_GP0_0_awaddr;
    wire [1:0]M_AXI_GP0_0_awburst;
    wire [3:0]M_AXI_GP0_0_awcache;
    wire [11:0]M_AXI_GP0_0_awid;
    wire [3:0]M_AXI_GP0_0_awlen;
    wire [1:0]M_AXI_GP0_0_awlock;
    wire [2:0]M_AXI_GP0_0_awprot;
    wire [3:0]M_AXI_GP0_0_awqos;
    wire M_AXI_GP0_0_awready;
    wire [2:0]M_AXI_GP0_0_awsize;
    wire M_AXI_GP0_0_awvalid;
    wire [11:0]M_AXI_GP0_0_bid;
    wire M_AXI_GP0_0_bready;
    wire [1:0]M_AXI_GP0_0_bresp;
    wire M_AXI_GP0_0_bvalid;
    wire [31:0]M_AXI_GP0_0_rdata;
    wire [11:0]M_AXI_GP0_0_rid;
    wire M_AXI_GP0_0_rlast;
    wire M_AXI_GP0_0_rready;
    wire [1:0]M_AXI_GP0_0_rresp;
    wire M_AXI_GP0_0_rvalid;
    wire [31:0]M_AXI_GP0_0_wdata;
    wire [11:0]M_AXI_GP0_0_wid;
    wire M_AXI_GP0_0_wlast;
    wire M_AXI_GP0_0_wready;
    wire [3:0]M_AXI_GP0_0_wstrb;
    wire M_AXI_GP0_0_wvalid;

    Axi3_read_if  #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp0_read ();
    Axi3_write_if #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp0_write ();
    Axi3_read_if  #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp1_read ();
    Axi3_write_if #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp1_write ();
    Axi3_read_if  #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp2_read ();
    Axi3_write_if #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp2_write ();
    Axi3_read_if  #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp3_read ();
    Axi3_write_if #(.ADDR_WIDTH(32),.DATA_WIDTH(64)) s_axi_hp3_write ();

    // HP1 is driven by apple_dma_engine inside apple_top.
    // HP2 read is dedicated to Disk II audio sample fetches.
    // HP2 write is driven by the SuperDuperDisplay bus-event egress
    // inside apple_top (axi_sdd_write).

    // tied-off HP outputs
    wire [5:0] s_axi_hp0_0_bid = 0;
    wire [5:0] s_axi_hp0_0_rid = 0;
    wire [5:0] s_axi_hp0_0_wid = 0;
    wire [5:0] s_axi_hp1_0_bid = 0;
    wire [5:0] s_axi_hp1_0_rid = 0;
    wire [5:0] s_axi_hp1_0_wid = 0;
    wire [5:0] s_axi_hp2_0_bid = 0;
    wire [5:0] s_axi_hp2_0_rid = 0;
    wire [5:0] s_axi_hp2_0_wid = 0;
    wire [5:0] s_axi_hp3_0_bid = 0;
    wire [5:0] s_axi_hp3_0_rid = 0;
    wire [5:0] s_axi_hp3_0_wid = 0;

    // Clocks and Resets from PS
    wire        fclk_clk0;              // PL Clock 0 (133 MHz)
    wire        fclk_clk1;              // IDELAY reference clock (~200 MHz from clk_wiz_1 clk_out2)
    wire        clk_pixel_148mhz;
    // 8 independent reset bits from proc_sys_reset_0 (C_NUM_PERP_ARESETN=8).
    // Each bit is the output of its own FDRE inside the IP — functionally
    // identical (same deassert cycle), but placeable near distinct load
    // clusters. Bit assignments documented in scripts/create_project.tcl
    // and consumed below.
    wire [7:0]  peripheral_133M_aresetn;          // PL Reset 0 (active low)
    wire        peripheral_148M_aresetn;

    // Framebuffer base-address register mirrored into the HP0 reader path.
    wire [31:0] fb_base_addr;

    wire audio_mclk_in = audio_i2s_mclk;
    wire audio_mclk_resetn;
    wire audio_bclk;
    wire audio_bclk_resetn;
    reg [1:0] audio_bclk_div_cnt = 2'b00;
    logic signed [15:0] mockingboard_audio_l;
    logic signed [15:0] mockingboard_audio_r;
    logic signed [15:0] disk2_audio_l;
    logic signed [15:0] disk2_audio_r;
    logic signed [15:0] mixed_audio_l;
    logic signed [15:0] mixed_audio_r;
    logic menu_chime_start;
    logic menu_chime_start_bclk;
    logic [19:0] menu_chime_count_q;
    logic [31:0] menu_chime_tone_step;
    logic [23:0] menu_chime_amplitude;
    logic [31:0] audio_sample_acc_q;
    logic audio_sample_resend;
    logic [47:0] mockingboard_audio_24_fclk;
    logic [47:0] mockingboard_audio_24_sampled_fclk;
    logic [47:0] mockingboard_audio_24_bclk;
    logic mock_i2s_lrck;
    logic mock_i2s_sdata;
    logic chime_i2s_lrck;
    logic chime_i2s_sdata;
    logic unused_mock_i2s_bclk;
    logic unused_chime_i2s_bclk;
    wire [32:0] audio_sample_acc_next =
        {1'b0, audio_sample_acc_q} + 33'd1546188;

    function automatic logic signed [15:0] sat_add16(
        input logic signed [15:0] a,
        input logic signed [15:0] b
    );
        logic signed [16:0] sum;
        begin
            sum = {a[15], a} + {b[15], b};
            if (sum > 17'sd32767)
                sat_add16 = 16'sh7FFF;
            else if (sum < -17'sd32768)
                sat_add16 = -16'sd32768;
            else
                sat_add16 = sum[15:0];
        end
    endfunction

    reset_sync reset_sync_audio_mclk_i (
        .clk    (audio_mclk_in),
        .arst_n (peripheral_133M_aresetn[6]),
        .srst_n (audio_mclk_resetn)
    );

    always_ff @(posedge audio_mclk_in or negedge audio_mclk_resetn) begin
        if (!audio_mclk_resetn)
            audio_bclk_div_cnt <= 2'b00;
        else
            audio_bclk_div_cnt <= audio_bclk_div_cnt + 2'b01;
    end
    assign audio_bclk = audio_bclk_div_cnt[1];

    localparam logic [19:0] MENU_CHIME_BCLK_75MS = 20'd230400;
    localparam logic [19:0] MENU_CHIME_BCLK_150MS = 20'd460800;
    localparam logic [19:0] MENU_CHIME_BCLK_225MS = 20'd691200;
    localparam logic [31:0] MENU_CHIME_TONE_A = 32'd78741067;   // 880 Hz at 48 kHz
    localparam logic [31:0] MENU_CHIME_TONE_B = 32'd118111601;  // 1320 Hz at 48 kHz

    cdc_pulse_toggle menu_chime_cdc_i (
        .src_clk(fclk_clk0),
        .src_resetn(peripheral_133M_aresetn[6]),
        .src_pulse(menu_chime_start),
        .dst_clk(audio_bclk),
        .dst_resetn(audio_bclk_resetn),
        .dst_pulse(menu_chime_start_bclk)
    );

    always_ff @(posedge audio_bclk or negedge audio_bclk_resetn) begin
        if (!audio_bclk_resetn) begin
            menu_chime_count_q <= 20'd0;
        end else if (menu_chime_start_bclk) begin
            menu_chime_count_q <= MENU_CHIME_BCLK_225MS;
        end else if (menu_chime_count_q != 20'd0) begin
            menu_chime_count_q <= menu_chime_count_q - 20'd1;
        end
    end

    always_comb begin
        if (menu_chime_count_q > MENU_CHIME_BCLK_150MS) begin
            menu_chime_tone_step = MENU_CHIME_TONE_A;
            menu_chime_amplitude = 24'h0009_0000;
        end else if (menu_chime_count_q > MENU_CHIME_BCLK_75MS) begin
            menu_chime_tone_step = MENU_CHIME_TONE_B;
            menu_chime_amplitude = 24'h0006_0000;
        end else begin
            menu_chime_tone_step = MENU_CHIME_TONE_B;
            menu_chime_amplitude = 24'h0002_4000;
        end
    end

    assign audio_i2s_bick = audio_bclk;
    assign audio_i2s_lrck = (menu_chime_count_q != 20'd0) ? chime_i2s_lrck : mock_i2s_lrck;
    assign audio_i2s_sdata = (menu_chime_count_q != 20'd0) ? chime_i2s_sdata : mock_i2s_sdata;

    reset_sync reset_sync_audio_bclk_i (
        .clk    (audio_bclk),
        .arst_n (audio_mclk_resetn),
        .srst_n (audio_bclk_resetn)
    );

    // Status signals after CDC into AXI/control domain (owned by pl_cdc_status)
    wire [31:0] frame_count_axi_sync2;
    wire        mmcm_locked_axi_sync2;
    wire [11:0] line_len_axi_sync2;
    wire [11:0] frame_lines_axi_sync2;

    // interrupts
    wire [15:0] IRQ;


    //==========================================================================
    // Zynq PS Instance
    //==========================================================================

    zynq_ps_bd_wrapper zynq_ps_i (
        // DDR and Fixed IO
        .DDR_addr            (DDR_Addr),
        .DDR_ba              (DDR_BankAddr),
        .DDR_cas_n           (DDR_CAS_n),
        .DDR_ck_n            (DDR_Clk_n),
        .DDR_ck_p            (DDR_Clk),
        .DDR_cke             (DDR_CKE),
        .DDR_cs_n            (DDR_CS_n),
        .DDR_dm              (DDR_DM),
        .DDR_dq              (DDR_DQ),
        .DDR_dqs_n           (DDR_DQS_n),
        .DDR_dqs_p           (DDR_DQS),
        .DDR_odt             (DDR_ODT),
        .DDR_ras_n           (DDR_RAS_n),
        .DDR_reset_n         (DDR_DRSTB),
        .DDR_we_n            (DDR_WEB),
        .FIXED_IO_ddr_vrn    (DDR_VRN),
        .FIXED_IO_ddr_vrp    (DDR_VRP),
        .FIXED_IO_mio        (MIO),
        .FIXED_IO_ps_clk     (PS_CLK),
        .FIXED_IO_ps_porb    (PS_PORB),
        .FIXED_IO_ps_srstb   (PS_SRSTB),


        .M_AXI_GP0_0_araddr (M_AXI_GP0_0_araddr),
        .M_AXI_GP0_0_arburst (M_AXI_GP0_0_arburst),
        .M_AXI_GP0_0_arcache (M_AXI_GP0_0_arcache),
        .M_AXI_GP0_0_arid (M_AXI_GP0_0_arid),
        .M_AXI_GP0_0_arlen (M_AXI_GP0_0_arlen),
        .M_AXI_GP0_0_arlock (M_AXI_GP0_0_arlock),
        .M_AXI_GP0_0_arprot (M_AXI_GP0_0_arprot),
        .M_AXI_GP0_0_arqos (M_AXI_GP0_0_arqos),
        .M_AXI_GP0_0_arready (M_AXI_GP0_0_arready),
        .M_AXI_GP0_0_arsize (M_AXI_GP0_0_arsize),
        .M_AXI_GP0_0_arvalid (M_AXI_GP0_0_arvalid),
        .M_AXI_GP0_0_awaddr (M_AXI_GP0_0_awaddr),
        .M_AXI_GP0_0_awburst (M_AXI_GP0_0_awburst),
        .M_AXI_GP0_0_awcache (M_AXI_GP0_0_awcache),
        .M_AXI_GP0_0_awid (M_AXI_GP0_0_awid),
        .M_AXI_GP0_0_awlen (M_AXI_GP0_0_awlen),
        .M_AXI_GP0_0_awlock (M_AXI_GP0_0_awlock),
        .M_AXI_GP0_0_awprot (M_AXI_GP0_0_awprot),
        .M_AXI_GP0_0_awqos (M_AXI_GP0_0_awqos),
        .M_AXI_GP0_0_awready (M_AXI_GP0_0_awready),
        .M_AXI_GP0_0_awsize (M_AXI_GP0_0_awsize),
        .M_AXI_GP0_0_awvalid (M_AXI_GP0_0_awvalid),
        .M_AXI_GP0_0_bid (M_AXI_GP0_0_bid),
        .M_AXI_GP0_0_bready (M_AXI_GP0_0_bready),
        .M_AXI_GP0_0_bresp (M_AXI_GP0_0_bresp),
        .M_AXI_GP0_0_bvalid (M_AXI_GP0_0_bvalid),
        .M_AXI_GP0_0_rdata (M_AXI_GP0_0_rdata),
        .M_AXI_GP0_0_rid (M_AXI_GP0_0_rid),
        .M_AXI_GP0_0_rlast (M_AXI_GP0_0_rlast),
        .M_AXI_GP0_0_rready (M_AXI_GP0_0_rready),
        .M_AXI_GP0_0_rresp (M_AXI_GP0_0_rresp),
        .M_AXI_GP0_0_rvalid (M_AXI_GP0_0_rvalid),
        .M_AXI_GP0_0_wdata (M_AXI_GP0_0_wdata),
        .M_AXI_GP0_0_wid (M_AXI_GP0_0_wid),
        .M_AXI_GP0_0_wlast (M_AXI_GP0_0_wlast),
        .M_AXI_GP0_0_wready (M_AXI_GP0_0_wready),
        .M_AXI_GP0_0_wstrb (M_AXI_GP0_0_wstrb),
        .M_AXI_GP0_0_wvalid (M_AXI_GP0_0_wvalid),

        // AXI HP0 Read Address Channel
        .S_AXI_HP0_0_araddr    (s_axi_hp0_read.araddr),
        .S_AXI_HP0_0_arburst   (s_axi_hp0_read.arburst),
        .S_AXI_HP0_0_arlen     (s_axi_hp0_read.arlen),
        .S_AXI_HP0_0_arready   (s_axi_hp0_read.arready),
        .S_AXI_HP0_0_arsize    (s_axi_hp0_read.arsize),
        .S_AXI_HP0_0_arvalid   (s_axi_hp0_read.arvalid),
        // AXI HP0 Read Data Channel
        .S_AXI_HP0_0_rdata     (s_axi_hp0_read.rdata),
        .S_AXI_HP0_0_rlast     (s_axi_hp0_read.rlast),
        .S_AXI_HP0_0_rready    (s_axi_hp0_read.rready),
        .S_AXI_HP0_0_rresp     (s_axi_hp0_read.rresp),
        .S_AXI_HP0_0_rvalid    (s_axi_hp0_read.rvalid),
        // AXI HP0 Write Address Channel (tied off — read-only)
        .S_AXI_HP0_0_awaddr    (s_axi_hp0_write.awaddr),
        .S_AXI_HP0_0_awburst   (s_axi_hp0_write.awburst),
        .S_AXI_HP0_0_awlen     (s_axi_hp0_write.awlen),
        .S_AXI_HP0_0_awready   (s_axi_hp0_write.awready),
        .S_AXI_HP0_0_awsize    (s_axi_hp0_write.awsize),
        .S_AXI_HP0_0_awvalid   (s_axi_hp0_write.awvalid),
        // AXI HP0 Write Data Channel (tied off)
        .S_AXI_HP0_0_wdata     (s_axi_hp0_write.wdata),
        .S_AXI_HP0_0_wlast     (s_axi_hp0_write.wlast),
        .S_AXI_HP0_0_wready    (s_axi_hp0_write.wready),
        .S_AXI_HP0_0_wstrb     (s_axi_hp0_write.wstrb),
        .S_AXI_HP0_0_wvalid    (s_axi_hp0_write.wvalid),
        // AXI HP0 Write Response Channel (tied off)
        .S_AXI_HP0_0_bready    (s_axi_hp0_write.bready),
        .S_AXI_HP0_0_bresp     (s_axi_hp0_write.bresp),
        .S_AXI_HP0_0_bvalid    (s_axi_hp0_write.bvalid),
        // tied-off AXI channels
        .S_AXI_HP0_0_arcache(0),
        .S_AXI_HP0_0_arid(0),
        .S_AXI_HP0_0_arlock(0),
        .S_AXI_HP0_0_arprot(0),
        .S_AXI_HP0_0_arqos(0),
        .S_AXI_HP0_0_awcache(0),
        .S_AXI_HP0_0_awid(0),
        .S_AXI_HP0_0_awlock(0),
        .S_AXI_HP0_0_awprot(0),
        .S_AXI_HP0_0_awqos(0),
        .S_AXI_HP0_0_bid(s_axi_hp0_0_bid),
        .S_AXI_HP0_0_rid(s_axi_hp0_0_rid),
        .S_AXI_HP0_0_wid(s_axi_hp0_0_wid),

        // AXI HP1 Read Address Channel (tied off — write-only)
        .S_AXI_HP1_0_araddr    (s_axi_hp1_read.araddr),
        .S_AXI_HP1_0_arburst   (s_axi_hp1_read.arburst),
        .S_AXI_HP1_0_arlen     (s_axi_hp1_read.arlen),
        .S_AXI_HP1_0_arready   (s_axi_hp1_read.arready),
        .S_AXI_HP1_0_arsize    (s_axi_hp1_read.arsize),
        .S_AXI_HP1_0_arvalid   (s_axi_hp1_read.arvalid),
        // AXI HP1 Read Data Channel (tied off)
        .S_AXI_HP1_0_rdata     (s_axi_hp1_read.rdata),
        .S_AXI_HP1_0_rlast     (s_axi_hp1_read.rlast),
        .S_AXI_HP1_0_rready    (s_axi_hp1_read.rready),
        .S_AXI_HP1_0_rresp     (s_axi_hp1_read.rresp),
        .S_AXI_HP1_0_rvalid    (s_axi_hp1_read.rvalid),
        // AXI HP1 Write Address Channel
        .S_AXI_HP1_0_awaddr    (s_axi_hp1_write.awaddr),
        .S_AXI_HP1_0_awburst   (s_axi_hp1_write.awburst),
        .S_AXI_HP1_0_awlen     (s_axi_hp1_write.awlen),
        .S_AXI_HP1_0_awready   (s_axi_hp1_write.awready),
        .S_AXI_HP1_0_awsize    (s_axi_hp1_write.awsize),
        .S_AXI_HP1_0_awvalid   (s_axi_hp1_write.awvalid),
        // AXI HP1 Write Data Channel
        .S_AXI_HP1_0_wdata     (s_axi_hp1_write.wdata),
        .S_AXI_HP1_0_wlast     (s_axi_hp1_write.wlast),
        .S_AXI_HP1_0_wready    (s_axi_hp1_write.wready),
        .S_AXI_HP1_0_wstrb     (s_axi_hp1_write.wstrb),
        .S_AXI_HP1_0_wvalid    (s_axi_hp1_write.wvalid),
        // AXI HP1 Write Response Channel
        .S_AXI_HP1_0_bready    (s_axi_hp1_write.bready),
        .S_AXI_HP1_0_bresp     (s_axi_hp1_write.bresp),
        .S_AXI_HP1_0_bvalid    (s_axi_hp1_write.bvalid),
        // tied-off AXI channels
        .S_AXI_HP1_0_arcache(0),
        .S_AXI_HP1_0_arid(0),
        .S_AXI_HP1_0_arlock(0),
        .S_AXI_HP1_0_arprot(0),
        .S_AXI_HP1_0_arqos(0),
        .S_AXI_HP1_0_awcache(0),
        .S_AXI_HP1_0_awid(0),
        .S_AXI_HP1_0_awlock(0),
        .S_AXI_HP1_0_awprot(0),
        .S_AXI_HP1_0_awqos(0),
        .S_AXI_HP1_0_bid(s_axi_hp1_0_bid),
        .S_AXI_HP1_0_rid(s_axi_hp1_0_rid),
        .S_AXI_HP1_0_wid(s_axi_hp1_0_wid),

        // AXI HP2 Read Address Channel (dedicated Disk II audio sample reads)
        .S_AXI_HP2_0_araddr    (s_axi_hp2_read.araddr),
        .S_AXI_HP2_0_arburst   (s_axi_hp2_read.arburst),
        .S_AXI_HP2_0_arlen     (s_axi_hp2_read.arlen),
        .S_AXI_HP2_0_arready   (s_axi_hp2_read.arready),
        .S_AXI_HP2_0_arsize    (s_axi_hp2_read.arsize),
        .S_AXI_HP2_0_arvalid   (s_axi_hp2_read.arvalid),
        // AXI HP2 Read Data Channel
        .S_AXI_HP2_0_rdata     (s_axi_hp2_read.rdata),
        .S_AXI_HP2_0_rlast     (s_axi_hp2_read.rlast),
        .S_AXI_HP2_0_rready    (s_axi_hp2_read.rready),
        .S_AXI_HP2_0_rresp     (s_axi_hp2_read.rresp),
        .S_AXI_HP2_0_rvalid    (s_axi_hp2_read.rvalid),
        // AXI HP2 Write Address Channel (unused)
        .S_AXI_HP2_0_awaddr    (s_axi_hp2_write.awaddr),
        .S_AXI_HP2_0_awburst   (s_axi_hp2_write.awburst),
        .S_AXI_HP2_0_awlen     (s_axi_hp2_write.awlen),
        .S_AXI_HP2_0_awready   (s_axi_hp2_write.awready),
        .S_AXI_HP2_0_awsize    (s_axi_hp2_write.awsize),
        .S_AXI_HP2_0_awvalid   (s_axi_hp2_write.awvalid),
        // AXI HP2 Write Data Channel (unused)
        .S_AXI_HP2_0_wdata     (s_axi_hp2_write.wdata),
        .S_AXI_HP2_0_wlast     (s_axi_hp2_write.wlast),
        .S_AXI_HP2_0_wready    (s_axi_hp2_write.wready),
        .S_AXI_HP2_0_wstrb     (s_axi_hp2_write.wstrb),
        .S_AXI_HP2_0_wvalid    (s_axi_hp2_write.wvalid),
        // AXI HP2 Write Response Channel (unused)
        .S_AXI_HP2_0_bready    (s_axi_hp2_write.bready),
        .S_AXI_HP2_0_bresp     (s_axi_hp2_write.bresp),
        .S_AXI_HP2_0_bvalid    (s_axi_hp2_write.bvalid),
        // tied-off AXI channels
        .S_AXI_HP2_0_arcache(0),
        .S_AXI_HP2_0_arid(0),
        .S_AXI_HP2_0_arlock(0),
        .S_AXI_HP2_0_arprot(0),
        .S_AXI_HP2_0_arqos(0),
        .S_AXI_HP2_0_awcache(0),
        .S_AXI_HP2_0_awid(0),
        .S_AXI_HP2_0_awlock(0),
        .S_AXI_HP2_0_awprot(0),
        .S_AXI_HP2_0_awqos(0),
        .S_AXI_HP2_0_bid(s_axi_hp2_0_bid),
        .S_AXI_HP2_0_rid(s_axi_hp2_0_rid),
        .S_AXI_HP2_0_wid(s_axi_hp2_0_wid),

        // AXI HP3 (Disk II DDR staging bridge)
        .S_AXI_HP3_0_araddr    (s_axi_hp3_read.araddr),
        .S_AXI_HP3_0_arburst   (s_axi_hp3_read.arburst),
        .S_AXI_HP3_0_arlen     (s_axi_hp3_read.arlen),
        .S_AXI_HP3_0_arready   (s_axi_hp3_read.arready),
        .S_AXI_HP3_0_arsize    (s_axi_hp3_read.arsize),
        .S_AXI_HP3_0_arvalid   (s_axi_hp3_read.arvalid),
        .S_AXI_HP3_0_rdata     (s_axi_hp3_read.rdata),
        .S_AXI_HP3_0_rlast     (s_axi_hp3_read.rlast),
        .S_AXI_HP3_0_rready    (s_axi_hp3_read.rready),
        .S_AXI_HP3_0_rresp     (s_axi_hp3_read.rresp),
        .S_AXI_HP3_0_rvalid    (s_axi_hp3_read.rvalid),
        .S_AXI_HP3_0_awaddr    (s_axi_hp3_write.awaddr),
        .S_AXI_HP3_0_awburst   (s_axi_hp3_write.awburst),
        .S_AXI_HP3_0_awlen     (s_axi_hp3_write.awlen),
        .S_AXI_HP3_0_awready   (s_axi_hp3_write.awready),
        .S_AXI_HP3_0_awsize    (s_axi_hp3_write.awsize),
        .S_AXI_HP3_0_awvalid   (s_axi_hp3_write.awvalid),
        .S_AXI_HP3_0_wdata     (s_axi_hp3_write.wdata),
        .S_AXI_HP3_0_wlast     (s_axi_hp3_write.wlast),
        .S_AXI_HP3_0_wready    (s_axi_hp3_write.wready),
        .S_AXI_HP3_0_wstrb     (s_axi_hp3_write.wstrb),
        .S_AXI_HP3_0_wvalid    (s_axi_hp3_write.wvalid),
        .S_AXI_HP3_0_bready    (s_axi_hp3_write.bready),
        .S_AXI_HP3_0_bresp     (s_axi_hp3_write.bresp),
        .S_AXI_HP3_0_bvalid    (s_axi_hp3_write.bvalid),
        .S_AXI_HP3_0_arcache(0),
        .S_AXI_HP3_0_arid(0),
        .S_AXI_HP3_0_arlock(0),
        .S_AXI_HP3_0_arprot(0),
        .S_AXI_HP3_0_arqos(0),
        .S_AXI_HP3_0_awcache(0),
        .S_AXI_HP3_0_awid(0),
        .S_AXI_HP3_0_awlock(0),
        .S_AXI_HP3_0_awprot(0),
        .S_AXI_HP3_0_awqos(0),
        .S_AXI_HP3_0_bid(s_axi_hp3_0_bid),
        .S_AXI_HP3_0_rid(s_axi_hp3_0_rid),
        .S_AXI_HP3_0_wid(s_axi_hp3_0_wid),

        // Fabric Clocks
        .FCLK_CLK0           (fclk_clk0),
        .FCLK_CLK1           (fclk_clk1),
        .clk_pixel_148mhz (clk_pixel_148mhz),
        .peripheral_133M_aresetn       (peripheral_133M_aresetn),  // [7:0] bus
        .peripheral_148M_aresetn       (peripheral_148M_aresetn),

        // interrupts
        .IRQ(IRQ)
    );


    /* SmartPort uses IRQ[1]. Every PL interrupt is pulse-stretched before
     * entering the Zynq PS. */
    logic smartport_irq;
    logic [15:0] irq_pulse_in;
    assign irq_pulse_in = {14'b0, smartport_irq, 1'b0};

    genvar gi;
    generate
        for (gi = 0; gi < 16; gi++) begin : irq_stretch
            irq_pulse_stretcher irq_pulse_stretcher_i (
                .clk(fclk_clk0),
                .rstn(peripheral_133M_aresetn[4]),
                .irq_pulse(irq_pulse_in[gi]),
                .irq_pulse_stretched(IRQ[gi])
            );
        end
    endgenerate


    wire converted_awlock = M_AXI_GP0_0_awlock != 2'b00;
    wire converted_arlock = M_AXI_GP0_0_arlock != 2'b00;

    globals::AxiSimple_common as_common;
    AxiSimple_if as_clients[7:0]();

    // as_clients[7] is the Applicard register file inside apple_top.

    axisimple_wrapper(
        .S_AXI_ACLK(fclk_clk0),
        .S_AXI_ARESETN(peripheral_133M_aresetn[4]),
        .S_AXI_AWVALID(M_AXI_GP0_0_awvalid),
        .S_AXI_AWREADY(M_AXI_GP0_0_awready),
        .S_AXI_AWID(M_AXI_GP0_0_awid),
		.S_AXI_AWADDR(M_AXI_GP0_0_awaddr),
		.S_AXI_AWLEN(M_AXI_GP0_0_awlen),
		.S_AXI_AWSIZE(M_AXI_GP0_0_awsize),
		.S_AXI_AWBURST(M_AXI_GP0_0_awburst),
		.S_AXI_AWLOCK(converted_awlock),
		.S_AXI_AWCACHE(M_AXI_GP0_0_awcache),
		.S_AXI_AWPROT(M_AXI_GP0_0_awprot),
		.S_AXI_AWQOS(M_AXI_GP0_0_awqos),
		.S_AXI_WVALID(M_AXI_GP0_0_wvalid),
		.S_AXI_WREADY(M_AXI_GP0_0_wready),
		.S_AXI_WDATA(M_AXI_GP0_0_wdata),
		.S_AXI_WSTRB(M_AXI_GP0_0_wstrb),
		.S_AXI_WLAST(M_AXI_GP0_0_wlast),
		.S_AXI_BVALID(M_AXI_GP0_0_bvalid),
		.S_AXI_BREADY(M_AXI_GP0_0_bready),
		.S_AXI_BID(M_AXI_GP0_0_bid),
		.S_AXI_BRESP(M_AXI_GP0_0_bresp),
        .S_AXI_ARVALID(M_AXI_GP0_0_arvalid),
		.S_AXI_ARREADY(M_AXI_GP0_0_arready),
		.S_AXI_ARID(M_AXI_GP0_0_arid),
		.S_AXI_ARADDR(M_AXI_GP0_0_araddr),
		.S_AXI_ARLEN(M_AXI_GP0_0_arlen),
		.S_AXI_ARSIZE(M_AXI_GP0_0_arsize),
		.S_AXI_ARBURST(M_AXI_GP0_0_arburst),
		.S_AXI_ARLOCK(converted_arlock),
		.S_AXI_ARCACHE(M_AXI_GP0_0_arcache),
		.S_AXI_ARPROT(M_AXI_GP0_0_arprot),
		.S_AXI_ARQOS(M_AXI_GP0_0_arqos),
        .S_AXI_RVALID(M_AXI_GP0_0_rvalid),
		.S_AXI_RREADY(M_AXI_GP0_0_rready),
		.S_AXI_RID(M_AXI_GP0_0_rid),
		.S_AXI_RDATA(M_AXI_GP0_0_rdata),
		.S_AXI_RLAST(M_AXI_GP0_0_rlast),
		.S_AXI_RRESP(M_AXI_GP0_0_rresp),
        .as_common(as_common),
        .as_clients(as_clients)
    );

    // DVI I2C (tristated - not used for basic test)
    assign dvi_scl = 1'bz;
    assign dvi_sda = 1'bz;

    logic apple_video_mode_50hz;
    logic apple_vblank_start_pulse;
    logic apple_reset_n_out;

    // these are unused, setting them to silence warnings
    assign a2ctrl_irq_n = 1'b1;
    assign a2ctrl_nmi_n = 1'b1;
    assign a2ctrl_rdy_n = 1'b1;
    assign a2ctrl_dma_n = 1'b1;
    assign a2ctrl_inh_n = 1'b1;
    assign a2ctrl_reset_n = apple_reset_n_out;
    assign a2fpga_oe_n_aux = 1'b1;

    wire [3:0] psram_oe;
    wire [3:0] psram_a_i;
    wire [3:0] psram_a_o;
    wire [3:0] psram_b_i;
    wire [3:0] psram_b_o;
    wire [7:0] eth_d_i;
    wire [7:0] eth_d_o;
    wire       eth_d_oe;

    apple_top apple_top_i (
        .clk(fclk_clk0),
        .rstn(peripheral_133M_aresetn[3:0]),
        .as_common(as_common),
        .as_client(as_clients[0]),
        .smartport_as_client(as_clients[2]),
        .boot_menu_as_client(as_clients[4]),
        .ps_dma_as_client(as_clients[3]),
        .mouse_as_client(as_clients[5]),
        .disk2_as_client(as_clients[6]),
        .applicard_as_client(as_clients[7]),
        .apple_data_pin(a2fpga_d),
        .apple_addr_pin(a2fpga_a),
        .apple_rw_pin(a2fpga_rdwr_n),
        .apple_phi0_pin(a2fpga_clk),
        .apple_m2sel_pin(a2fpga_m2sel),
        .apple_m2b0_pin(a2fpga_m2b0),
        .apple_inh_pin(a2fpga_inh_n),
        .apple_res_pin(a2fpga_reset_n),
        .apple_irq_pin(a2fpga_irq_n),
        .apple_rdy_pin(a2fpga_rdy_n),
        .apple_dma_pin(a2fpga_dma_n),
        .apple_nmi_pin(a2fpga_nmi_n),
        .tini_oe_pin(a2fpga_oe_n),
        .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(a2fpga_dir_a),
        .tini_data_dir_pin(a2fpga_dir_d),
        .video_mode_50hz_out(apple_video_mode_50hz),
        .apple_vblank_start_pulse(apple_vblank_start_pulse),
        .smartport_irq(smartport_irq),
        .apple_reset_n_out(apple_reset_n_out),
        .mockingboard_audio_l(mockingboard_audio_l),
        .mockingboard_audio_r(mockingboard_audio_r),
        .disk2_audio_l(disk2_audio_l),
        .disk2_audio_r(disk2_audio_r),
        .menu_chime_start(menu_chime_start),
        .audio_sample_tick(audio_sample_resend),
        .axi_hp1_read(s_axi_hp1_read),
        .axi_hp1_write(s_axi_hp1_write),
        .axi_audio_read(s_axi_hp2_read),
        .axi_sdd_write(s_axi_hp2_write),
        /* Dedicated Apple-cycle egress path. */
        .axi_hp0_write(s_axi_hp0_write),
        .axi_hp3_read(s_axi_hp3_read),
        .axi_hp3_write(s_axi_hp3_write),
        /* Capture every Apple bus cycle. The PS renderer derives frame
         * boundaries from line/cycle wrap and decides which completed
         * frames the compositor presents. */
        .frame_en(1'b1),
        .psram_ce_n(psram_ce_n),
        .psram_clk(psram_clk),
        .psram_oe(psram_oe),
        .psram_a_o(psram_a_o),
        .psram_b_o(psram_b_o),
        .psram_a_i(psram_a_i),
        .psram_b_i(psram_b_i),
        .eth_d_i(eth_d_i),
        .eth_d_o(eth_d_o),
        .eth_d_oe(eth_d_oe),
        .eth_a(eth_a),
        .eth_rd_n(eth_rd_n),
        .eth_wr_n(eth_wr_n),
        .eth_cs_n(eth_cs_n),
        .eth_rst_n(eth_rst_n),
        .eth_int_n(eth_int_n)
    );

    video_top video_top_i (
        .clk(fclk_clk0),
        .resetn(peripheral_133M_aresetn[5]),
        .as_common(as_common),
        .as_client(as_clients[1]),
        .pixel_clk(clk_pixel_148mhz),
        .pixel_resetn(peripheral_148M_aresetn),
        .apple_video_mode_50hz(apple_video_mode_50hz),
        .apple_vblank_start_pulse(apple_vblank_start_pulse),
        .axi_read_if(s_axi_hp0_read),
        .dvi_red(dvi_red),
        .dvi_grn(dvi_grn),
        .dvi_blu(dvi_blu),
        .dvi_clk(dvi_clk),
        .dvi_de(dvi_de),
        .dvi_hsync(dvi_hsync),
        .dvi_vsync(dvi_vsync)
    );

    always_ff @(posedge fclk_clk0 or negedge peripheral_133M_aresetn[6]) begin
        if (!peripheral_133M_aresetn[6]) begin
            audio_sample_acc_q <= 32'd0;
            audio_sample_resend <= 1'b0;
            mockingboard_audio_24_sampled_fclk <= 48'd0;
        end else begin
            audio_sample_acc_q <= audio_sample_acc_next[31:0];
            audio_sample_resend <= audio_sample_acc_next[32];
            if (audio_sample_acc_next[32])
                mockingboard_audio_24_sampled_fclk <= mockingboard_audio_24_fclk;
        end
    end

    assign mixed_audio_l = sat_add16(mockingboard_audio_l, disk2_audio_l);
    assign mixed_audio_r = sat_add16(mockingboard_audio_r, disk2_audio_r);
    assign mockingboard_audio_24_fclk = {
        mixed_audio_l, 8'h00,
        mixed_audio_r, 8'h00
    };

    cdc_handshake_reg #(.WIDTH(48)) mockingboard_audio_cdc_i (
        .wr_clk(fclk_clk0),
        .wr_rstn(peripheral_133M_aresetn[6]),
        .din(mockingboard_audio_24_sampled_fclk),
        .resend(audio_sample_resend),
        .rd_clk(audio_bclk),
        .rd_rstn(audio_bclk_resetn),
        .dout(mockingboard_audio_24_bclk)
    );

    audio_i2s_pcm_tx audio_i2s_pcm_tx_i (
        .bclk_in      (audio_bclk),
        .resetn       (audio_bclk_resetn),
        .enable       (1'b1),
        .mute         (1'b0),
        .sample_l     (mockingboard_audio_24_bclk[47:24]),
        .sample_r     (mockingboard_audio_24_bclk[23:0]),
        .i2s_bclk     (unused_mock_i2s_bclk),
        .i2s_lrck     (mock_i2s_lrck),
        .i2s_sdata    (mock_i2s_sdata)
    );

    audio_i2s_tone menu_chime_i2s_i (
        .bclk_in      (audio_bclk),
        .resetn       (audio_bclk_resetn),
        .enable       (menu_chime_count_q != 20'd0),
        .mute         (1'b0),
        .tone_step    (menu_chime_tone_step),
        .amplitude    (menu_chime_amplitude),
        .i2s_bclk     (unused_chime_i2s_bclk),
        .i2s_lrck     (chime_i2s_lrck),
        .i2s_sdata    (chime_i2s_sdata)
    );

    audio_spdif_pcm_tx audio_spdif_pcm_tx_i (
        .clk       (fclk_clk0),
        .resetn    (peripheral_133M_aresetn[6]),
        .enable    (1'b1),
        .mute      (1'b0),
        .sample_l  (mockingboard_audio_24_sampled_fclk[47:24]),
        .sample_r  (mockingboard_audio_24_sampled_fclk[23:0]),
        .spdif_out (audio_spdif_out)
    );

    // Keep IDELAYCTRL alive for the IDELAYE2-based PSRAM capture path even
    // though the design does not otherwise consume the RDY output.
    (* keep = "true" *) wire idelayctrl_rdy;

    // PSRAM IOBUF — infer at top level, breaking out into i, o, and oe
    genvar i;
    for (i = 0; i < 4; i++) begin
        assign psram_a_io[i] = psram_oe[i] ? psram_a_o[i] : 1'hz;
        assign psram_b_io[i] = psram_oe[i] ? psram_b_o[i] : 1'hz;
        assign psram_a_i[i] = psram_a_io[i];
        assign psram_b_i[i] = psram_b_io[i]; 
    end

    genvar eth_i;
    generate
        for (eth_i = 0; eth_i < 8; eth_i++) begin : eth_data_iobuf
            assign eth_d[eth_i] = eth_d_oe ? eth_d_o[eth_i] : 1'bz;
            assign eth_d_i[eth_i] = eth_d[eth_i];
        end
    endgenerate


    (* DONT_TOUCH = "true" *)
    IDELAYCTRL IDELAYCTRL_inst(
        .RDY(idelayctrl_rdy),
        .REFCLK(fclk_clk1),
        .RST(!peripheral_133M_aresetn[7]) // async reset, so a foreign reset is fine
    );


endmodule
