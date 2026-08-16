module apple_top(
    input clk,
    // 4-bit reset bus from proc_sys_reset_0 (C_NUM_PERP_ARESETN=8). Each bit
    // is functionally identical (same source, same deassert cycle) but the
    // placer can put each driver flop near its load cluster, breaking up
    // what would otherwise be a single high-fanout reset broadcast.
    //   [0] memory subsystem (psram_simple, psram_driver, dma_engine,
    //       cycle_capture, cycle_egress)
    //   [1] apple bus path (bus_wrapper, soft_switch_manager,
    //       timing_gen, write_arbiter)
    //   [2] card emulation (mockingboard, smartport, boot_menu, no_slot_clock)
    //   [3] axisimple shim + control register block (ps_dma_command,
    //       card_feature_enable, reset_release_ready, etc.)
    input [3:0] rstn,
    input globals::AxiSimple_common as_common,
    input [31:0] as_vtw_phasor_wdata,
    input [3:0] as_vtw_phasor_wstrb,
    AxiSimple_if.client as_client,
    AxiSimple_if.client smartport_as_client,
    AxiSimple_if.client boot_menu_as_client,
    AxiSimple_if.client ps_dma_as_client,
    AxiSimple_if.client mouse_as_client,
    AxiSimple_if.client disk2_as_client,
    AxiSimple_if.client applicard_as_client,
    inout [7:0] apple_data_pin,
    inout [15:0] apple_addr_pin,
    inout apple_rw_pin,
    input apple_phi0_pin,
    input apple_7m_pin,
    input apple_q3_pin,
    input apple_m2sel_pin,
    input apple_m2b0_pin,
    input apple_devsel_n_pin,
    inout apple_inh_pin,
    inout apple_res_pin,
    inout apple_irq_pin,
    inout apple_rdy_pin,
    inout apple_dma_pin,
    inout apple_nmi_pin,
    output reg tini_oe_pin,
    input tini_5v_pin,
    output reg tini_addr_dir_pin,
    output reg tini_data_dir_pin,
    output logic video_mode_50hz_out,
    output logic apple_vblank_start_pulse,
    output logic smartport_irq,
    output logic apple_reset_n_out,
    output logic signed [15:0] mockingboard_audio_l,
    output logic signed [15:0] mockingboard_audio_r,
    output logic signed [15:0] disk2_audio_l,
    output logic signed [15:0] disk2_audio_r,
    output logic menu_chime_start,
    input  logic audio_sample_tick,

    // HP1 AXI3 master to PS DDR (used by apple_dma_engine)
    Axi3_read_if.master  axi_hp1_read,
    Axi3_write_if.master axi_hp1_write,

    // HP2 AXI3 read master to PS DDR (dedicated to Disk II audio sample fetch)
    Axi3_read_if.master  axi_audio_read,

    // HP2 AXI3 write master to PS DDR (dedicated to the SuperDuperDisplay
    // bus-event egress; the HP2 read channel above is independent)
    Axi3_write_if.master axi_sdd_write,

    // HP0 AXI3 write master to PS DDR (used by apple_cycle_egress)
    Axi3_write_if.master axi_hp0_write,

    // HP3 AXI3 masters to PS DDR for Disk II track staging. Keeping tracks in
    // DDR leaves the complete 8 MB PSRAM address space available to RamWorks.
    Axi3_read_if.master  axi_hp3_read,
    Axi3_write_if.master axi_hp3_write,

    // Frame-render gate: 1 when current Apple video frame should be streamed
    // to the PS, 0 when the frame is being skipped.
    input  logic                           frame_en,

    // PSRAM physical pins
    output wire psram_ce_n,
    output wire psram_clk,
    output wire [3:0] psram_oe,
    output wire [3:0] psram_a_o,
    output wire [3:0] psram_b_o,
    input wire [3:0] psram_a_i,
    input wire [3:0] psram_b_i,

    // W5100S Ethernet controller parallel MCU bus
    input  wire [7:0] eth_d_i,
    output wire [7:0] eth_d_o,
    output wire       eth_d_oe,
    output wire [1:0] eth_a,
    output wire       eth_rd_n,
    output wire       eth_wr_n,
    output wire       eth_cs_n,
    output wire       eth_rst_n,
    input  wire       eth_int_n
);

    globals::AppleBus_read  ab_read;
    globals::AppleBus_read  physical_ab_read;
    globals::AppleBus_read  virtual_ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write ab_write_arb;
    globals::SoftSwitchState sss;
    logic ramworks_en_q;  // RamWorks 8 MB expansion, card-control register 0x62

    // ONE//e stand-alone selection. This build has no Apple-slot power-sense
    // pin, so the guard's level veto is tied low and bit 13 of CARD_CTRL 0x5B
    // reports that limit. All available raw clocks and control pins still feed
    // the activity interlock.
    localparam logic ONEE_POWER_SENSE_PRESENT = 1'b0;
    logic onee_request_q;
    logic onee_enable_effective;
    logic onee_force_outputs_off;
    logic physical_bus_isolate;
    logic onee_physical_isolation_hold;
    logic onee_activity_now;
    logic onee_activity_lockout;
    logic onee_reselect_armed;
    logic onee_selected;
    logic onee_activity_quiet;
    logic [2:0] onee_inhibit_reason;
    logic virtual_req_ready;
    logic virtual_resp_valid;
    logic [7:0] virtual_resp_rdata;

    onee_mode_safety_guard onee_mode_safety_guard_i (
        .clk                    (clk),
        .resetn                 (rstn[1]),
        .manual_enable_request  (onee_request_q),
        .apple_power_present_raw(1'b0),
        .apple_phi0_raw         (apple_phi0_pin),
        .apple_7m_raw           (apple_7m_pin),
        .apple_q3_raw           (apple_q3_pin),
        .apple_m2sel_raw        (apple_m2sel_pin),
        .apple_m2b0_raw         (apple_m2b0_pin),
        .apple_devsel_n_raw     (apple_devsel_n_pin),
        .apple_reset_n_raw      (apple_res_pin),
        .apple_inh_n_raw        (apple_inh_pin),
        .apple_irq_n_raw        (apple_irq_pin),
        .apple_nmi_n_raw        (apple_nmi_pin),
        .apple_rdy_n_raw        (apple_rdy_pin),
        .apple_dma_n_raw        (apple_dma_pin),
        .onee_enable_effective  (onee_enable_effective),
        .force_outputs_off      (onee_force_outputs_off),
        .physical_bus_isolate   (physical_bus_isolate),
        .physical_isolation_hold(onee_physical_isolation_hold),
        .apple_activity_now     (onee_activity_now),
        .apple_activity_lockout (onee_activity_lockout),
        .reselect_armed         (onee_reselect_armed),
        .onee_selected          (onee_selected),
        .apple_activity_quiet   (onee_activity_quiet),
        .inhibit_reason         (onee_inhibit_reason)
    );

    apple_virtual_bus apple_virtual_bus_i (
        .clk              (clk),
        .resetn           (rstn[1]),
        .res_n_in         (1'b1),
        .irq_n_in         (1'b1),
        .nmi_n_in         (1'b1),
        .rdy_n_in         (1'b1),
        .dma_n_in         (1'b1),
        .inh_n_in         (1'b1),
        .req_valid        (1'b0),
        .req_ready        (virtual_req_ready),
        .req_addr         (16'hFFFF),
        .req_rw           (1'b1),
        .req_wdata        (8'h00),
        .resp_valid       (virtual_resp_valid),
        .resp_rdata       (virtual_resp_rdata),
        .floating_bus_data(8'hFF),
        .ab_write         (ab_write_arb),
        .ab_read          (virtual_ab_read)
    );

    // Isolation asserts on the raw request before the guard may select the
    // virtual bus. The physical wrapper receives no requests while isolated;
    // its own direct kill also clears every pin driver without waiting for
    // another Apple clock edge.
    assign ab_read  = onee_enable_effective ? virtual_ab_read
                                             : physical_ab_read;
    assign ab_write = physical_bus_isolate ? '0 : ab_write_arb;

    soft_switch_manager ssm(
        .clk(clk),
        .rstn(rstn[1]),
        .ramworks_en(ramworks_en_q),
        .ab_read(ab_read),
        .sss(sss)
    );

    // Timing generator signals
    logic apple_bus_pulse;
    logic video_mode_50hz;
    logic set_frame_zero_pulse;
    logic set_vblank_start_pulse;
    logic apple_vblank_lock_seen_q;
    logic apple_reset_prev_q;
    logic apple_reset_release_q;
    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    logic       video_mode_50hz_detected_q;
    logic       video_mode_50hz_valid_q;
    logic [6:0] detect_cycle_in_line;
    logic [15:0] detect_line_clk_count;
    logic mouse_vblank_start_pulse;
    logic bm_vbl_cmd_pulse;   // boot ROM CMD_VBL_START (software-paced)
    localparam [15:0] APPLE_LINE_PERIOD_50HZ_THRESHOLD = 16'd8513;

    assign apple_bus_pulse = ab_read.sss_en;
    assign video_mode_50hz = video_mode_50hz_valid_q && video_mode_50hz_detected_q;
    assign video_mode_50hz_out = video_mode_50hz;
    assign mouse_vblank_start_pulse =
        update_pulse && (line_in_frame == 9'd192) && (cycle_in_line == 7'd0);
    /* HDMI genlock heartbeat. video_timing_gen uses this pulse ONLY for
     * activity detection ("is the Apple running?"), never for phase, so it
     * must be steady whenever PHI0 ticks. The boot ROM's CMD_VBL_START used
     * to feed it directly, but a II/II+ paces that by vapor-lock at one
     * pulse per ~80-200 ms -- far outside the detector's 2-frame tolerance
     * -- so V_TOTAL flapped 1123<->1125 through the whole boot menu (monitor
     * re-lock flicker). Source it from the PL frame flywheel instead: one
     * tick per Apple frame, host-independent, and genlock now also survives
     * the boot->OS handoff instead of stepping to the standard mode. */
    assign apple_vblank_start_pulse = mouse_vblank_start_pulse;
    wire apple_reset_assert_pulse = rstn[1] && apple_reset_prev_q && !ab_read.res;
    assign set_frame_zero_pulse = apple_reset_release_q;
    // The ROM's first VBL command after reset is the calibrated lock point.
    // Later commands must not re-phase the Apple timing counters with raw
    // polling jitter (the genlock heartbeat no longer needs them at all).
    assign set_vblank_start_pulse =
        bm_vbl_cmd_pulse && !apple_vblank_lock_seen_q;

    localparam logic [7:0] CARD_CTRL_REG_SLOT_ENABLE_MASK   = 8'h00;
    localparam logic [7:0] CARD_CTRL_REG_FEATURE_ENABLE_MASK = 8'h01;
    localparam logic [7:0] CARD_CTRL_REG_SOFTSW_STATE        = 8'h02;
    localparam logic [7:0] CARD_CTRL_REG_RESET_RELEASE       = 8'h03;
    localparam logic [7:0] CARD_CTRL_REG_NSC_TIME_LO         = 8'h04;
    localparam logic [7:0] CARD_CTRL_REG_NSC_TIME_HI         = 8'h05;
    localparam logic [7:0] CARD_CTRL_REG_NSC_WRITE_SEQ       = 8'h06;
    localparam logic [7:0] CARD_CTRL_REG_MENU_CHIME          = 8'h07;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_PAN_LO       = 8'h08;
    localparam logic [7:0] CARD_CTRL_REG_APPLE_RESET_STATUS  = 8'h09;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_PAN_HI       = 8'h0A;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_AUDIO        = 8'h0C;
    localparam logic [7:0] CARD_CTRL_REG_DISK2_SOUND_BASE    = 8'h10;
    localparam logic [7:0] CARD_CTRL_REG_DISK2_SOUND_CONTROL = 8'h11;
    // SuperSprite (TMS9918 VDP) PS-facing readback window.
    localparam logic [7:0] CARD_CTRL_REG_SS_REGS_LO   = 8'h40; // R0..R3
    localparam logic [7:0] CARD_CTRL_REG_SS_REGS_HI   = 8'h41; // R4..R7
    localparam logic [7:0] CARD_CTRL_REG_SS_STATUS    = 8'h42; // status/frame/switches
    localparam logic [7:0] CARD_CTRL_REG_SS_VRAM_DATA = 8'h43; // read VRAM[addr]
    localparam logic [7:0] CARD_CTRL_REG_SS_VRAM_ADDR = 8'h44; // set VRAM read addr
    localparam logic [7:0] CARD_CTRL_REG_SS_SPR_FLAGS = 8'h45; // PS sprite status
    localparam logic [7:0] CARD_CTRL_REG_ETH_ADDR     = 8'h46; // W5100S host access address
    localparam logic [7:0] CARD_CTRL_REG_ETH_DATA     = 8'h47; // W5100S host access byte
    localparam logic [7:0] CARD_CTRL_REG_ETH_CMD      = 8'h48; // bit0 go, bit1 write
    localparam logic [7:0] CARD_CTRL_REG_ETH_STATUS   = 8'h49; // ready/busy/done/error + read byte
    // Virtual SSC printer FIFO drain window (ssc_card in slot 1).
    localparam logic [7:0] CARD_CTRL_REG_SSC_STATUS   = 8'h4A; // [11:0] count, [16] overflow, [17] enabled
    localparam logic [7:0] CARD_CTRL_REG_SSC_HEAD     = 8'h4B; // [7:0] oldest byte, [8] valid
    localparam logic [7:0] CARD_CTRL_REG_SSC_CTRL     = 8'h4C; // bit0 pop, bit1 clear, bit2 overflow clear
    localparam logic [7:0] CARD_CTRL_REG_SSC_ACIA     = 8'h4D; // [7:0] command, [15:8] control latch
    // ONE//e stand-alone request/status. Write bit 0 to request the mode.
    // Read: [0] request, [1] effective, [2] physical isolate, [3] forced off,
    // [4] live activity, [5] lockout, [6] quiet, [7] reselect armed,
    // [8] selected, [9] isolation hold, [12:10] inhibit reason,
    // [13] power-sense pin fitted (0 on this board), [14] sensed power,
    // [15] ONE//e HDL present, [31:24] register signature/version 8'hE1.
    localparam logic [7:0] CARD_CTRL_REG_ONEE         = 8'h5B;
    localparam logic [7:0] CARD_CTRL_REG_VTW_WR_CHECK = 8'h36;
    localparam logic [7:0] CARD_CTRL_REG_VTW_WR_ADDR  = 8'h37;
    localparam logic [7:0] CARD_CTRL_REG_VTW_C000_CTX = 8'h38;
    localparam logic [7:0] CARD_CTRL_REG_VTW_C000_CNT = 8'h39;
    // Virtual TransWarp accelerator (vtw_core_top) control/status window.
    //   VTW_CTRL        : bit0 enable request (gated on //e or II/II+ mode),
    //                     bit1 core_run, [3:2] speed_mode (0=full, 1=divided,
    //                     2=1MHz-locked), bit4 assert Apple RES# (takeover
    //                     machine reset), bit5 II+ Apple-key synthesis,
    //                     bit6 ignore all $C074 speed-switch writes,
    //                     bit7 disable the private Disk II read shortcut,
    //                     [31:16] pace divider (divided mode).
    //   VTW_SHADOW_ADDR : 18-bit shadow port-B pointer; writing it also
    //                     fetches that byte for VTW_SHADOW_DATA reads.
    //   VTW_SHADOW_DATA : write = store byte at pointer, pointer++ and
    //                     refetch (boot ROM copy is one pointer write then a
    //                     byte stream); read = last fetched byte.
    //   VTW_SHADOW_DATA4: write = queue four little-endian bytes.
    //   VTW_SHADOW_DATA4_STATUS: {ready,busy,accepted_count[29:0]}.
    //   VTW_SHADOW_READ4: write bit0 = fetch four bytes and advance.
    //   VTW_SHADOW_READ4_DATA: last fetched little-endian word.
    //   VTW_SHADOW_READ4_STATUS: {ready,busy,completed_count[29:0]}.
    //   VTW_SYNC_CMD    : [15:0] Apple address, [23:16] write data, [24]
    //                     rw (1=read). Write issues one real bus cycle via
    //                     the vTW engine; honored only while the core is
    //                     held (core_run=0) -- the boot-time ROM copy path.
    //   VTW_SYNC_STATUS : [7:0] read data, [8] busy, [9] done (sticky,
    //                     cleared by the next VTW_SYNC_CMD write).
    //   VTW_STATUS      : [1:0] $C074 state, [2] bus owned, [3] effective
    //                     enable, [4] core_run echo, [31:16] core PC.
    localparam logic [7:0] CARD_CTRL_REG_VTW_CTRL        = 8'h70;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_ADDR = 8'h71;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_DATA = 8'h72;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SYNC_CMD    = 8'h73;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SYNC_STATUS = 8'h74;
    localparam logic [7:0] CARD_CTRL_REG_VTW_STATUS      = 8'h75;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CNT_CORE    = 8'h76;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CNT_BUS     = 8'h77;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CNT_POSTED  = 8'h78;
    localparam logic [7:0] CARD_CTRL_REG_VTW_POST_STATS  = 8'h79;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CNT_INVALID = 8'h7A;
    //   VTW_LAST_SYNC   : [15:0] address, [23:16] data, [24] rw of the
    //                     last completed vTW bus cycle (bench forensics).
    //   VTW_CXXX_RING_* : last eight $C1xx-$CFFF bus cycles, two 16-bit
    //                     addresses per register, newest first.
    localparam logic [7:0] CARD_CTRL_REG_VTW_LAST_SYNC   = 8'h7B;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CXXX_RING0  = 8'h7C;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CXXX_RING1  = 8'h7D;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CXXX_RING2  = 8'h7E;
    localparam logic [7:0] CARD_CTRL_REG_VTW_CXXX_RING3  = 8'h7F;
    // Event-frozen vTW forensic trace (`busdbg clear` re-arms).
    localparam logic [7:0] CARD_CTRL_REG_VTW_TRACE_STATUS = 8'h80;
    localparam logic [7:0] CARD_CTRL_REG_VTW_BUS_FAULTS   = 8'h81;
    localparam logic [7:0] CARD_CTRL_REG_VTW_IO_TRACE0    = 8'h82;
    localparam logic [7:0] CARD_CTRL_REG_VTW_PC_TRACE0    = 8'h92;
    // CPU0 video-post injection: PUSH={8'b0,data[7:0],addr[15:0]}.
    // STATUS={ready,accepted_count[30:0]}; each accepted PUSH increments it.
    localparam logic [7:0] CARD_CTRL_REG_VTW_POST_PUSH     = 8'h9A;
    localparam logic [7:0] CARD_CTRL_REG_VTW_POST_STATUS   = 8'h9B;
    // RamWorks flush+hold: bit0 freezes the vTW core and flushes its line
    // cache; bit1 releases the frozen core after the PS-DMA copy.
    // STATUS={busy,held,count[29:0]}.
    localparam logic [7:0] CARD_CTRL_REG_VTW_RW_FLUSH      = 8'h9C;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_DATA4  = 8'h9D;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_DATA4_STATUS = 8'h9E;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_READ4 = 8'h9F;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_READ4_DATA = 8'hA0;
    localparam logic [7:0] CARD_CTRL_REG_VTW_SHADOW_READ4_STATUS = 8'hA1;
    //   VTW_C0_RING_*   : last eight $C00x/$C01x soft-switch cycles with
    //                     latched data ({rw,addr[4:0],data[7:0]} x2/reg).
    localparam logic [7:0] CARD_CTRL_REG_VTW_C0_RING0    = 8'h6C;
    localparam logic [7:0] CARD_CTRL_REG_VTW_C0_RING1    = 8'h6D;
    localparam logic [7:0] CARD_CTRL_REG_VTW_C0_RING2    = 8'h6E;
    localparam logic [7:0] CARD_CTRL_REG_VTW_C0_RING3    = 8'h6F;
    // Per-region slowdown config: [8:0] region enables, [31:16] duration.
    localparam logic [7:0] CARD_CTRL_REG_VTW_SLOWDOWN    = 8'h6B;
    localparam int unsigned CARD_CTRL_FEATURE_NSC_ENABLE_BIT = 0;
    localparam int unsigned CARD_CTRL_FEATURE_SS_ENABLE_BIT  = 1;
    localparam int unsigned CARD_CTRL_FEATURE_SSC_ENABLE_BIT = 2;
    localparam int unsigned CARD_CTRL_DISK2_SOUND_EVENT_SHIFT = 16;
    localparam logic [31:0] CARD_CTRL_DISK2_SOUND_EVENT_MASK  = 32'h000F_0000;
    localparam logic [31:0] CARD_CTRL_ETH_CMD_GO              = 32'h0000_0001;
    localparam logic [31:0] CARD_CTRL_ETH_CMD_WRITE           = 32'h0000_0002;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_READY        = 32'h0000_0001;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_BUSY         = 32'h0000_0002;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_DONE         = 32'h0000_0004;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_ERROR        = 32'h0000_0008;
    localparam logic [31:0] RESET_RELEASE_CPU0_READY         = 32'h0000_0001;
    localparam logic [31:0] RESET_RELEASE_CPU1_READY         = 32'h0000_0002;
    localparam logic [31:0] RESET_RELEASE_READY_MASK =
        RESET_RELEASE_CPU0_READY | RESET_RELEASE_CPU1_READY;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_RESET      = 32'h0000_0016;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_VALID_MASK = 32'h0000_007E;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_REQUIRED   = 32'h0000_0080;
    localparam logic [47:0] PHASOR_PAN_RESET                 = 48'h5B5B5B5B5B5B;
    localparam logic [31:0] PHASOR_AUDIO_RESET               = 32'h0204_0000;

    function automatic logic [31:0] card_slot_enable_normalize(input logic [31:0] value);
        card_slot_enable_normalize =
            (value & CARD_CTRL_SLOT_ENABLE_VALID_MASK) | CARD_CTRL_SLOT_ENABLE_REQUIRED;
    endfunction

    logic [31:0] card_slot_enable_mask_q =
        CARD_CTRL_SLOT_ENABLE_RESET | CARD_CTRL_SLOT_ENABLE_REQUIRED;
    logic [31:0] card_feature_enable_mask_q = 32'h0000_0000;
    logic [31:0] reset_release_ready_q = 32'h0000_0000;
    logic menu_chime_start_q = 1'b0;
    logic [47:0] phasor_pan_q = PHASOR_PAN_RESET;
    logic [31:0] phasor_audio_q = PHASOR_AUDIO_RESET;
    logic [15:0] eth_host_addr_q = 16'h0000;
    logic [7:0]  eth_host_wdata_q = 8'h00;
    logic [7:0]  eth_host_rdata_q = 8'h00;
    logic        eth_host_req_pulse = 1'b0;
    logic        eth_host_write_q = 1'b0;
    logic        eth_host_busy_q = 1'b0;
    logic        eth_host_done_q = 1'b0;
    logic        eth_host_error_q = 1'b0;
    logic        eth_host_ready;
    logic        eth_host_done;
    logic        eth_host_error;
    logic [7:0]  eth_host_rdata;
    logic        ssc_tx_pop_pulse = 1'b0;
    logic        ssc_tx_clear_pulse = 1'b0;
    logic        ssc_tx_ovf_clear_pulse = 1'b0;
    logic [11:0] ssc_tx_count;
    logic [7:0]  ssc_tx_head;
    logic        ssc_tx_head_valid;
    logic        ssc_tx_overflow;
    logic [7:0]  ssc_acia_command;
    logic [7:0]  ssc_acia_control;
    logic [31:0] disk2_sound_sample_base_q = 32'h0000_0000;
    logic [31:0] disk2_sound_control_q = 32'h0000_0000; // bit 0 enable, [11:8] volume 0..10
    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) disk2_sound_read();
    localparam logic [63:0] NSC_TIME_RESET =
        {8'h26, 8'h01, 8'h01, 8'h01, 8'h00, 8'h00, 8'h00, 8'h00};
    logic [63:0] nsc_time_shadow_q = NSC_TIME_RESET;
    logic [63:0] nsc_time_bcd_q = NSC_TIME_RESET;
    logic [31:0] nsc_write_seq_q = 32'h0000_0000;
    logic [7:0] apple_reset_seq_q = 8'h00;
    logic [13:0] ss_vram_addr_q = 14'd0;    // PS-set VRAM read address
    logic [6:0]  ss_status_flags_q = 7'd0;  // PS-set sprite status flags
    wire card_slot1_enable = card_slot_enable_mask_q[1];
    wire card_slot2_enable = card_slot_enable_mask_q[2];
    wire card_slot4_enable = card_slot_enable_mask_q[4];
    wire card_slot5_enable = card_slot_enable_mask_q[5];
    wire card_slot6_enable = card_slot_enable_mask_q[6];
    // SuperSprite hardware and software require slot 7, which is also the
    // SmartPort slot. Enabling SuperSprite therefore gates off SmartPort, SD
    // storage, and the SmartPort GETDIB detection channel.
    wire card_supersprite_enable =
        card_feature_enable_mask_q[CARD_CTRL_FEATURE_SS_ENABLE_BIT];
    wire no_slot_clock_enabled =
        card_feature_enable_mask_q[CARD_CTRL_FEATURE_NSC_ENABLE_BIT];
    /* The SSC shares slot 1 with the Uthernet II (disjoint decode), so it
     * enables through its own feature bit instead of the slot mask. */
    wire card_ssc_enable =
        card_feature_enable_mask_q[CARD_CTRL_FEATURE_SSC_ENABLE_BIT];
    wire [7:0] no_slot_clock_slot_mask =
        (card_slot2_enable ? 8'h04 : 8'h00) |
        (card_slot4_enable ? 8'h10 : 8'h00) |
        (card_slot6_enable ? 8'h40 : 8'h00) |
        8'h80;
    wire apple_reset_release =
        (reset_release_ready_q & RESET_RELEASE_READY_MASK) == RESET_RELEASE_READY_MASK;
    /* A2CTRL.RESET is the sole card-generated RESET assertion path. Merge
     * every virtual-card request with the power-up/boot hold so RESET uses
     * the populated open-collector transistor on every Apple model. The
     * separate A2FPGA.RESET lane remains an input observer. */
    assign apple_reset_n_out = physical_bus_isolate ? 1'b1 :
        (apple_reset_release && !ab_write_arb.assert_res);
    assign menu_chime_start = menu_chime_start_q;
    logic smartport_active;
    logic disk2_active;
    logic disk2_active_timing_q;
    logic boot_target_disk2;
    logic vtw_core_run_eff_q;
    logic vtw_disk2_boot_scan_q;
    logic vtw_disk2_active;
    logic vtw_d2_req_valid;
    logic [3:0] vtw_d2_req_addr;
    logic vtw_d2_req_ready;
    logic vtw_d2_resp_valid;
    logic [7:0] vtw_d2_resp_rdata;
    logic vtw_d2_cycle_tick;
    logic vtw_d2_native_cycle_active;
    logic vtw_d2_time_ready;
    logic vtw_d2_write_timing_active;
    logic disk2_sound_spinning;
    logic [7:0] disk2_sound_qtrack;
    logic [3:0] disk2_sound_event;
    logic [7:0] disk2_sound_seek_start_qtrack;
    logic [7:0] disk2_sound_seek_distance;
    logic [3:0] disk2_menu_sound_event_q;
    logic [2:0] boot_menu_slot;
    logic       boot_menu_slot_valid;
    wire [20:0] current_softswitch_state = {
        sss.sw_ramworks_bank,
        sss.sw_lcram_write,
        sss.sw_lcram_read,
        sss.sw_lcram_bank2,
        sss.sw_dhires,
        sss.sw_80col,
        sss.sw_altcharset,
        sss.sw_hires,
        sss.sw_page2,
        sss.sw_mixed,
        sss.sw_text,
        sss.sw_altzp,
        sss.sw_ramwrt,
        sss.sw_ramrd,
        sss.sw_80store
    };

    always_ff @(posedge clk) begin
        if (!rstn[1]) begin
            apple_reset_prev_q          <= 1'b1;
            apple_reset_release_q       <= 1'b0;
            apple_vblank_lock_seen_q    <= 1'b0;
            detect_cycle_in_line       <= 7'd0;
            detect_line_clk_count      <= 16'd0;
            video_mode_50hz_detected_q <= 1'b0;
            video_mode_50hz_valid_q    <= 1'b0;
        end else begin
            // Apple RES# is not a PSRAM coherency boundary. Cache contents
            // remain valid across CTRL+RESET.
            apple_reset_release_q     <= !apple_reset_prev_q && ab_read.res;
            apple_reset_prev_q <= ab_read.res;
            detect_line_clk_count <= detect_line_clk_count + 16'd1;

            if (!ab_read.res) begin
                apple_vblank_lock_seen_q <= 1'b0;
            end else if (set_vblank_start_pulse) begin
                apple_vblank_lock_seen_q <= 1'b1;
            end

            if (apple_bus_pulse) begin
                if (detect_cycle_in_line == 7'd64) begin
                    detect_cycle_in_line       <= 7'd0;
                    detect_line_clk_count      <= 16'd0;
                    video_mode_50hz_detected_q <=
                        ((detect_line_clk_count + 16'd1) >= APPLE_LINE_PERIOD_50HZ_THRESHOLD);
                    video_mode_50hz_valid_q    <= 1'b1;
                end else begin
                    detect_cycle_in_line <= detect_cycle_in_line + 7'd1;
                end
            end
        end
    end

    apple_timing_gen apple_timing_gen_i (
        .clk(clk),
        .resetn(rstn[1]),
        .apple_bus_pulse(apple_bus_pulse),
        .video_mode_50hz(video_mode_50hz),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(set_frame_zero_pulse),
        .set_vblank_start_pulse(set_vblank_start_pulse)
    );

    /* psram_simple owns the physical PSRAM command interface. */
    logic        mc_psram_valid;
    logic        mc_psram_ready;
    logic [7:0]  mc_psram_cmd;
    logic [23:0] mc_psram_addr;
    logic [63:0] mc_psram_wdata;
    logic        mc_psram_rvalid;
    logic [63:0] mc_psram_rdata;

    // PS DMA line interface, driven by apple_dma_engine.
    logic [20:0] mc_dma_line_addr;
    logic        mc_dma_rw;
    logic [63:0] mc_dma_wdata;
    logic        mc_dma_valid;
    logic        mc_dma_ready;
    logic [63:0] mc_dma_rdata;
    logic        mc_dma_rvalid;
    logic [20:0] mc_disk2_line_addr;
    logic        mc_disk2_rw;
    logic [63:0] mc_disk2_wdata;
    logic [7:0]  mc_disk2_wstrb;
    logic        mc_disk2_valid;
    logic        mc_disk2_ready;
    logic [63:0] mc_disk2_rdata;
    logic        mc_disk2_rvalid;

    // ------------------------------------------------------------------
    // Apple bus cycle capture + PS DDR egress (stages 1 & 2a).
    // capture FIFO is consumed internally by egress; no top-level FIFO ports.
    // ------------------------------------------------------------------
    apple_cycle_capture_pkg::AppleCycleRecord cycle_capture_data_internal;
    logic                                     cycle_capture_empty_internal;
    logic                                     cycle_capture_rd_en_internal;
    logic                                     capture_drop_sticky_internal;
    logic                                     overlay_capture_drop_internal;
    logic                                     overlay_devsel_enabled;
    logic                                     overlay_capture_armed;
    logic                                     overlay_capture_bank_aux;
    logic [15:0]                              overlay_capture_base;
    logic [15:0]                              overlay_capture_limit;

    logic        egress_cfg_enable_q;
    logic [31:0] egress_cfg_ring_base_q;
    logic [4:0]  egress_cfg_ring_size_log2_q;
    logic [31:0] egress_cfg_producer_ptr_addr_q;
    logic [31:0] egress_cfg_consumer_ptr_q;
    logic        egress_cfg_reset_pulse;

    logic [31:0] egress_stat_producer_ptr;
    logic [31:0] egress_stat_records_written;
    logic [31:0] egress_stat_gap_markers;
    logic [31:0] egress_stat_bursts_issued;
    logic [31:0] egress_stat_full_stall_cycles;

    logic        egress_capture_drop_ack;

    logic shr_capture_active_w;
    apple_cycle_capture apple_cycle_capture_i (
        .clk(clk),
        .resetn(rstn[0]),
        .soft_reset(!ab_read.res),
        .ab_read(ab_read),
        .sss(sss),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .frame_en(frame_en),
        .overlay_devsel_enabled(overlay_devsel_enabled),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(overlay_capture_bank_aux),
        .overlay_capture_base(overlay_capture_base),
        .overlay_capture_limit(overlay_capture_limit),
        .cycle_capture_data(cycle_capture_data_internal),
        .cycle_capture_rd_en(cycle_capture_rd_en_internal),
        .cycle_capture_empty(cycle_capture_empty_internal),
        .capture_drop_sticky(capture_drop_sticky_internal),
        .capture_drop_ack(egress_capture_drop_ack),
        .overlay_capture_drop_sticky(overlay_capture_drop_internal),
        .shr_capture_active(shr_capture_active_w)
    );

    apple_cycle_egress apple_cycle_egress_i (
        .clk                       (clk),
        .resetn                    (rstn[0] && !egress_cfg_reset_pulse),
        .cycle_capture_data        (cycle_capture_data_internal),
        .cycle_capture_empty       (cycle_capture_empty_internal),
        .cycle_capture_rd_en       (cycle_capture_rd_en_internal),
        .capture_drop_sticky       (capture_drop_sticky_internal),
        .capture_drop_ack          (egress_capture_drop_ack),
        .cfg_enable                (egress_cfg_enable_q),
        .cfg_ring_base_addr        (egress_cfg_ring_base_q),
        .cfg_ring_size_log2        (egress_cfg_ring_size_log2_q),
        .cfg_producer_ptr_addr     (egress_cfg_producer_ptr_addr_q),
        .cfg_consumer_ptr          (egress_cfg_consumer_ptr_q),
        .stat_producer_ptr         (egress_stat_producer_ptr),
        .stat_records_written      (egress_stat_records_written),
        .stat_gap_markers          (egress_stat_gap_markers),
        .stat_bursts_issued        (egress_stat_bursts_issued),
        .stat_full_stall_cycles    (egress_stat_full_stall_cycles),
        .axi_hp0_write             (axi_hp0_write)
    );

    // ------------------------------------------------------------------
    // SuperDuperDisplay full-rate bus-event tap + dedicated egress.
    // Independent of the renderer capture path: its own FIFO, its own
    // DDR ring, its own AXI port (HP2 write). Enabled only while the PS
    // SDD streaming service is active.
    // ------------------------------------------------------------------
    apple_cycle_capture_pkg::AppleCycleRecord sdd_tap_data_internal;
    logic                                     sdd_tap_empty_internal;
    logic                                     sdd_tap_rd_en_internal;
    logic                                     sdd_tap_drop_sticky_internal;
    logic                                     sdd_egress_drop_ack;

    logic        sdd_cfg_enable_q;
    logic [31:0] sdd_cfg_ring_base_q;
    logic [4:0]  sdd_cfg_ring_size_log2_q;
    logic [31:0] sdd_cfg_producer_ptr_addr_q;
    logic [31:0] sdd_cfg_consumer_ptr_q;
    logic        sdd_cfg_reset_pulse;

    /* Machine mode reported by the boot ROM: 0=unknown, 1=II/II+,
     * 2=IIe, 3=IIgs. Unknown and IIgs modes prohibit INH and DMA; the PS
     * must positively identify a compatible machine before either signal
     * can be driven. */
    logic [1:0]  machine_mode_q;
    /* AUX_PROVIDE: card serves the aux 64K + RamWorks banks from PSRAM.
     * Frontend policy enables this only for a //e with no physical aux card;
     * the fabric interlock itself permits INH on identified //e and II/II+
     * hosts. Reset = 0: snoop-only, GS-safe. */
    logic        aux_provide_en_q;
    /* PSRAM read-capture delay: [4:0] delay taps, [5] sample edge.
     * Writing the register pulses the driver's load strobe. */
    logic [4:0]  psram_dcount_q;
    logic        psram_dcount_edge_q;
    logic        psram_dcount_wr_pulse_q;
    logic        machine_inh_allowed;
    /* Give the wrapper a same-edge copy that the placer can keep near its
     * loads. The wrapper still sees each mode change on its original edge. */
    (* DONT_TOUCH = "TRUE" *) logic machine_inh_allowed_wrapper_q;
    logic        machine_m2sel_active_high;
    logic        machine_gs_m2_qualify;
    logic        machine_is_iiplus;
    assign machine_inh_allowed = (machine_mode_q == 2'd1) ||
                                 (machine_mode_q == 2'd2);
    /* M2SEL qualification arms only on a POSITIVE GS identification.
     * It must NOT apply in UNKNOWN mode: the //e's level on the M2SEL
     * pin is not guaranteed to read "asserted", and qualifying before
     * identification could invalidate every cycle -- the boot ROM
     * could then never serve, the machine ID could never be reported,
     * and the card would fail to boot on supported Apple II/IIe hosts.
     * Pre-ID exposure on a GS is the few milliseconds of its own ROM
     * slot scan; the INH/DMA interlock covers the serving hazards in
     * that window. */
    assign machine_gs_m2_qualify = (machine_mode_q == 2'd3);

    /* II/II+ mode selects only the proven host-specific electrical behavior
     * (served-byte output hold and accelerated Apple-key synthesis). Bus
     * capture uses the same fixed sample point on every host. */
    assign machine_is_iiplus = (machine_mode_q == 2'd1);

    logic [31:0] sdd_stat_producer_ptr;
    logic [31:0] sdd_stat_records_written;
    logic [31:0] sdd_stat_gap_markers;
    logic [31:0] sdd_stat_bursts_issued;
    logic [31:0] sdd_stat_full_stall_cycles;

    sdd_bus_tap sdd_bus_tap_i (
        .clk                      (clk),
        .resetn                   (rstn[0]),
        .enable                   (sdd_cfg_enable_q),
        .ab_read                  (ab_read),
        .route_info               ({
            (sss.route_kind == globals::APPLE_ROUTE_CACHE) && sss.addr_decode_en,
            sss.addr_decode[23:16] != 8'd0,
            sss.route_kind == globals::APPLE_ROUTE_ROM
        }),
        .cycle_capture_data       (sdd_tap_data_internal),
        .cycle_capture_empty      (sdd_tap_empty_internal),
        .cycle_capture_rd_en      (sdd_tap_rd_en_internal),
        .capture_drop_sticky      (sdd_tap_drop_sticky_internal),
        .capture_drop_ack         (sdd_egress_drop_ack)
    );

    apple_cycle_egress sdd_cycle_egress_i (
        .clk                       (clk),
        .resetn                    (rstn[0] && !sdd_cfg_reset_pulse),
        .cycle_capture_data        (sdd_tap_data_internal),
        .cycle_capture_empty       (sdd_tap_empty_internal),
        .cycle_capture_rd_en       (sdd_tap_rd_en_internal),
        .capture_drop_sticky       (sdd_tap_drop_sticky_internal),
        .capture_drop_ack          (sdd_egress_drop_ack),
        .cfg_enable                (sdd_cfg_enable_q),
        .cfg_ring_base_addr        (sdd_cfg_ring_base_q),
        .cfg_ring_size_log2        (sdd_cfg_ring_size_log2_q),
        .cfg_producer_ptr_addr     (sdd_cfg_producer_ptr_addr_q),
        .cfg_consumer_ptr          (sdd_cfg_consumer_ptr_q),
        .stat_producer_ptr         (sdd_stat_producer_ptr),
        .stat_records_written      (sdd_stat_records_written),
        .stat_gap_markers          (sdd_stat_gap_markers),
        .stat_bursts_issued        (sdd_stat_bursts_issued),
        .stat_full_stall_cycles    (sdd_stat_full_stall_cycles),
        .axi_hp0_write             (axi_sdd_write)
    );

    logic [31:0] dbg_apple_access_count;
    logic [31:0] dbg_apple_miss_count;
    logic [31:0] dbg_aux_write_count;
    logic [31:0] dbg_dma_admit_count;
    logic [31:0] dbg_dma_complete_count;
    logic [31:0] dbg_serve_late_count;
    logic [31:0] dbg_serve_max_latency;
    logic [31:0] dbg_wq_drop_count;
    logic [15:0] dbg_wq_state;
    logic [23:0] dbg_miss_ctx;
    logic        res_filtered_dbg;
    logic [31:0] dbg_lost_cycle_count;

    // Physical PSRAM command/data driver.
    psram_driver psram_driver_i (
        .clk(clk),
        .resetn(rstn[0]),
        .valid(mc_psram_valid),
        .ready(mc_psram_ready),
        .cmd(mc_psram_cmd),
        .addr(mc_psram_addr),
        .wdata(mc_psram_wdata),
        .rvalid(mc_psram_rvalid),
        .rdata(mc_psram_rdata),
        .done(),
        /* The PS boot-time eye scan programs CARD_CTRL 0x63 to center
         * the read-capture point across board and temperature variation. */
        .dcount_wr_en(psram_dcount_wr_pulse_q),
        .dcount_wr(psram_dcount_q),
        .dcount_edge(psram_dcount_edge_q),
        .psram_oe(psram_oe),
        .psram_a_i(psram_a_i),
        .psram_a_o(psram_a_o),
        .psram_b_i(psram_b_i),
        .psram_b_o(psram_b_o),
        .psram_ce_n(psram_ce_n),
        .psram_clk(psram_clk)
    );

    // individual writer interfaces that get arbited into the mux
    globals::AppleBus_write brain_transplant_write;
    globals::AppleBus_write mb1_ab_write;
    globals::AppleBus_write mouse_ab_write;
    // Interrupt-chain debug taps (declared here so the card instantiations
    // above the counter block can drive them; see the counters near the
    // mouse_card instance).
    logic [3:0]  mouse_dbg_mode;
    logic        mouse_dbg_vbl_pending;
    logic        mouse_dbg_irq_pending;
    logic        mb1_dbg_ssi_irq;
    logic        mb1_dbg_ssi_backend_done;
    logic        mb1_dbg_ssi_enable_ints;
    globals::AppleBus_write applicard_ab_write;
    globals::AppleBus_write uthernet_ab_write;
    globals::AppleBus_write ssc_ab_write;
    globals::AppleBus_write disk2_ab_write;
    globals::AppleBus_write smartport_ab_write;
    globals::AppleBus_write boot_menu_ab_write;
    globals::AppleBus_write no_slot_clock_ab_write;
    globals::AppleBus_write supersprite_ab_write;
    logic signed [15:0] ss_psg_audio;   // SuperSprite AY-3-8910 output
    logic signed [15:0] mb1_audio_l;    // mockingboard pre-mix (SS PSG summed in)
    logic signed [15:0] mb1_audio_r;
    // SuperSprite PS-facing readback (driven by supersprite_card_i).
    logic [63:0] ss_regs;
    logic [7:0]  ss_status;
    logic [15:0] ss_frame;
    logic        ss_apple_video;
    logic        ss_vdp_overlay;
    logic [7:0]  ss_vram_data;
    logic [63:0] nsc_write_time_bcd;
    logic        nsc_write_time_strobe;

    /* Bus-capture forensics (II+ ghost-write investigation). Cleared by
     * writing CARD_CTRL 0x2D; read via 0x2D-0x30 (see decode below). */
    logic [31:0] busdbg_quality;
    logic [31:0] busdbg_tap_mismatch;
    logic [31:0] busdbg_strobe_anom;
    logic [31:0] busdbg_tap_last;
    logic [31:0] busdbg_ghost_write;
    logic        busdbg_clear_pulse;
    logic        vtw_iiplus_dma_refresh_active;
    apple_bus_wrapper apple_bus_wrapper_i (
        .res_filtered_out(res_filtered_dbg),
        .dbg_lost_cycle_count(dbg_lost_cycle_count),
        .dbg_bus_quality(busdbg_quality),
        .dbg_tap_mismatch(busdbg_tap_mismatch),
        .dbg_strobe_anom(busdbg_strobe_anom),
        .dbg_tap_last(busdbg_tap_last),
        .dbg_ghost_write(busdbg_ghost_write),
        .dbg_clear(busdbg_clear_pulse),
        .clk(clk),
        .rstn(rstn[1]),
        .physical_bus_isolate(physical_bus_isolate),
        .inh_allowed(machine_inh_allowed_wrapper_q),
        .gs_m2_qualify(machine_gs_m2_qualify),
        .m2sel_active_high(machine_m2sel_active_high),
        .host_is_iiplus(machine_is_iiplus),
        .iiplus_dma_refresh_active(vtw_iiplus_dma_refresh_active),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(apple_phi0_pin),
        .apple_m2sel_pin(apple_m2sel_pin),
        .apple_m2b0_pin(apple_m2b0_pin),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(tini_5v_pin), // hardcoded to 0 (active low)
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(physical_ab_read),
        .ab_write(ab_write)
    );

    /* PSRAM serves aux/RamWorks when frontend policy enables AUX_PROVIDE
     * and provides the PS DMA staging port. Main RAM, language-card RAM,
     * and Disk II track staging are owned by other memory paths. */
    /* vTW RamWorks line port (vtw_core_top <-> psram_simple). */
    logic        vtw_bus_owned;
    logic        vtw_enable_eff;
    logic        vtw_video_phase_1mhz;
    logic        vtw_rw_req_valid;
    logic        vtw_rw_req_rw;
    logic [23:0] vtw_rw_req_addr;
    logic [63:0] vtw_rw_req_wline;
    logic        vtw_rw_req_ready;
    logic        vtw_rw_resp_valid;
    logic [63:0] vtw_rw_resp_rline;

    psram_simple psram_simple_i (
        .clk(clk),
        .resetn(rstn[0]),
        .ab_read(ab_read),
        .sss(sss),
        .aux_provide_en(aux_provide_en_q),
        .vtw_bus_owned(vtw_bus_owned),
        .ab_write(brain_transplant_write),
        .dma_line_addr(mc_dma_line_addr),
        .dma_rw(mc_dma_rw),
        .dma_wdata(mc_dma_wdata),
        .dma_valid(mc_dma_valid),
        .dma_ready(mc_dma_ready),
        .dma_rdata(mc_dma_rdata),
        .dma_rvalid(mc_dma_rvalid),
        .vtw_valid(vtw_rw_req_valid),
        .vtw_rw(vtw_rw_req_rw),
        .vtw_addr(vtw_rw_req_addr),
        .vtw_wline(vtw_rw_req_wline),
        .vtw_ready(vtw_rw_req_ready),
        .vtw_rvalid(vtw_rw_resp_valid),
        .vtw_rline(vtw_rw_resp_rline),
        .psram_valid(mc_psram_valid),
        .psram_ready(mc_psram_ready),
        .psram_cmd(mc_psram_cmd),
        .psram_addr(mc_psram_addr),
        .psram_wdata(mc_psram_wdata),
        .psram_rvalid(mc_psram_rvalid),
        .psram_rdata(mc_psram_rdata),
        .dbg_aux_read_count(dbg_apple_access_count),
        .dbg_aux_write_count(dbg_aux_write_count),
        .dbg_deadline_miss_count(dbg_apple_miss_count),
        .dbg_dma_admit_count(dbg_dma_admit_count),
        .dbg_dma_complete_count(dbg_dma_complete_count),
        .dbg_serve_late_count(dbg_serve_late_count),
        .dbg_serve_max_latency(dbg_serve_max_latency),
        .dbg_wq_drop_count(dbg_wq_drop_count),
        .dbg_wq_state(dbg_wq_state),
        .dbg_miss_ctx(dbg_miss_ctx)
    );
    /* Disk II line staging bridge over HP3 DDR. */
    disk2_ddr_bridge disk2_ddr_bridge_i (
        .clk(clk),
        .rstn(rstn[0]),
        .mc_line_addr(mc_disk2_line_addr),
        .mc_rw(mc_disk2_rw),
        .mc_wdata(mc_disk2_wdata),
        .mc_wstrb(mc_disk2_wstrb),
        .mc_valid(mc_disk2_valid),
        .mc_ready(mc_disk2_ready),
        .mc_rdata(mc_disk2_rdata),
        .mc_rvalid(mc_disk2_rvalid),
        .axi_read(axi_hp3_read),
        .axi_write(axi_hp3_write)
    );
    /* Reset forensics: no-reset sticky flops that latch any post-arm
     * dip of the replicated rstn bits or an Apple-reset assertion
     * (post-filter). Writing CARD_CTRL 0x64 clears/arms; reading it returns
     * {external_res_seen, internal_res_seen, res_seen, rstn_dips[3:0]}.
     * The source bits sample the merged Appletini reset request while the
     * filtered line is low, distinguishing our deliberate open-collector
     * pull from a motherboard/keyboard-originated reset. Deliberately never
     * reset -- they must survive the very events they record. */
    logic [3:0]  rstn_dip_sticky = 4'b0;
    logic        res_seen_sticky = 1'b0;
    logic        res_internal_seen_sticky = 1'b0;
    logic        res_external_seen_sticky = 1'b0;
    logic        res_filtered_prev_q = 1'b1;
    logic        forensics_clear_pulse;
    always_ff @(posedge clk) begin
        if (forensics_clear_pulse) begin
            rstn_dip_sticky <= 4'b0;
            res_seen_sticky <= 1'b0;
            res_internal_seen_sticky <= 1'b0;
            res_external_seen_sticky <= 1'b0;
            res_filtered_prev_q <= res_filtered_dbg;
        end else begin
            res_filtered_prev_q <= res_filtered_dbg;
            rstn_dip_sticky <= rstn_dip_sticky | ~rstn;
            if (!res_filtered_dbg) begin
                res_seen_sticky <= 1'b1;
            end
            if (res_filtered_prev_q && !res_filtered_dbg) begin
                if (ab_write_arb.assert_res) begin
                    res_internal_seen_sticky <= 1'b1;
                end
                else begin
                    res_external_seen_sticky <= 1'b1;
                end
            end
        end
    end
    /* Disable a card by suppressing its phase strobes. Assigning slot 0 is
     * invalid because its slot-I/O decode aliases the $C080-$C08F language-
     * card switches and can create phantom device traffic. */
    function automatic globals::AppleBus_read gate_ab(
        input globals::AppleBus_read ab,
        input logic en
    );
        globals::AppleBus_read g;
        g = ab;
        if (!en) begin
            g.addr_en       = 1'b0;
            g.data_en       = 1'b0;
            g.sss_en        = 1'b0;
            g.serve_en      = 1'b0;
        end
        return g;
    endfunction

    localparam logic [2:0] MB1_SLOT_ASSIGN = 3'h4;

    mockingboard mb1(
        .clk(clk),
        .rstn(rstn[2]),
        .slot_assign(MB1_SLOT_ASSIGN),
        .pan(phasor_pan_q),
        .audio_control(phasor_audio_q),
        .audio_sample_tick(audio_sample_tick),
        .sss(sss),
        .ab_read(gate_ab(ab_read, card_slot4_enable)),
        .ab_write(mb1_ab_write),
        .audio_l(mb1_audio_l),
        .audio_r(mb1_audio_r),
        .dbg_ssi_irq(mb1_dbg_ssi_irq),
        .dbg_ssi_backend_done(mb1_dbg_ssi_backend_done),
        .dbg_ssi_enable_ints(mb1_dbg_ssi_enable_ints)
    );

    // Sum the SuperSprite PSG into the card-audio bus with saturation. The PSG
    // output is pre-scaled with headroom; halve it so a full Mockingboard mix
    // plus PSG cannot wrap.
    //
    // Registered (one fclk stage) to pipeline the audio summation: the
    // downstream mixer (appletini_yarz_top) adds disk2 audio with another
    // saturating stage and only samples on an audio tick (~48 kHz), so the
    // whole PSG->mix->sample chain was a single ~14-level combinational path
    // on the 133 MHz clock. Splitting it here halves the depth; the added
    // ~7.5 ns of latency on a ~48 kHz sample stream is inaudible.
    wire signed [16:0] ss_mix_l = mb1_audio_l + (ss_psg_audio >>> 1);
    wire signed [16:0] ss_mix_r = mb1_audio_r + (ss_psg_audio >>> 1);
    logic signed [15:0] mockingboard_audio_l_q;
    logic signed [15:0] mockingboard_audio_r_q;
    always_ff @(posedge clk) begin
        if (!rstn[2]) begin
            mockingboard_audio_l_q <= 16'sd0;
            mockingboard_audio_r_q <= 16'sd0;
        end else begin
            mockingboard_audio_l_q <=
                (ss_mix_l >  17'sd32767) ?  16'sd32767 :
                (ss_mix_l < -17'sd32768) ? -16'sd32768 : ss_mix_l[15:0];
            mockingboard_audio_r_q <=
                (ss_mix_r >  17'sd32767) ?  16'sd32767 :
                (ss_mix_r < -17'sd32768) ? -16'sd32768 : ss_mix_r[15:0];
        end
    end
    assign mockingboard_audio_l = mockingboard_audio_l_q;
    assign mockingboard_audio_r = mockingboard_audio_r_q;

    mouse_card mouse_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .vblank_start_pulse(mouse_vblank_start_pulse),
        .ab_read(gate_ab(ab_read, card_slot2_enable)),
        .sss(sss),
        .slot_assign(3'h2),
        .as_common(as_common),
        .as_client(mouse_as_client),
        .ab_write(mouse_ab_write),
        .dbg_mode(mouse_dbg_mode),
        .dbg_vbl_pending(mouse_dbg_vbl_pending),
        .dbg_irq_pending(mouse_dbg_irq_pending)
    );

    // ------------------------------------------------------------------
    // Interrupt-chain debug counters (II+ mouse/Phasor freeze diagnosis).
    // All taps are off the vTW critical path. Saturating 8-bit counts of
    // each link in the two interrupt chains, plus mode/flag snapshots,
    // so the UART can see exactly where a chain goes dead. Read via
    // registers 0x2B (mouse) and 0x2C (Phasor/SSI).
    //   mouse chain: VBL pulse -> vbl_pending -> mode-enabled -> assert_irq
    //   speech chain: phoneme plays -> backend_done -> enable_ints -> direct_irq
    // (Debug tap wires are declared above, next to the card ab_write nets.)
    // ------------------------------------------------------------------
    logic [7:0]  irqdbg_mouse_vbl_q;
    logic [7:0]  irqdbg_mouse_irq_q;
    logic [7:0]  irqdbg_ssi_backend_q;
    logic [7:0]  irqdbg_ssi_irq_q;
    logic        irqdbg_mouse_irq_prev_q;
    logic        irqdbg_ssi_backend_prev_q;
    logic        irqdbg_ssi_irq_prev_q;
    // CPU IRQ/BRK-vector fetches: the 6502 reads $FFFE (then $FFFF) whenever it
    // takes an IRQ or executes BRK. Counting $FFFE reads tells us whether the
    // CPU services ANY interrupt during a freeze (climbing) or has interrupts
    // masked and never vectors (flat). serve_en is one pulse per bus cycle.
    logic [7:0]  irqdbg_vec_q;

    always_ff @(posedge clk) begin
        if (!rstn[2]) begin
            irqdbg_mouse_vbl_q        <= 8'd0;
            irqdbg_mouse_irq_q        <= 8'd0;
            irqdbg_ssi_backend_q      <= 8'd0;
            irqdbg_ssi_irq_q          <= 8'd0;
            irqdbg_vec_q              <= 8'd0;
            irqdbg_mouse_irq_prev_q   <= 1'b0;
            irqdbg_ssi_backend_prev_q <= 1'b0;
            irqdbg_ssi_irq_prev_q     <= 1'b0;
        end
        else begin
            irqdbg_mouse_irq_prev_q   <= mouse_ab_write.assert_irq;
            irqdbg_ssi_backend_prev_q <= mb1_dbg_ssi_backend_done;
            irqdbg_ssi_irq_prev_q     <= mb1_dbg_ssi_irq;
            if (ab_read.serve_en && ab_read.rw &&
                (ab_read.addr == 16'hFFFE) && irqdbg_vec_q != 8'hFF) begin
                irqdbg_vec_q <= irqdbg_vec_q + 8'd1;
            end
            if (mouse_vblank_start_pulse && irqdbg_mouse_vbl_q != 8'hFF) begin
                irqdbg_mouse_vbl_q <= irqdbg_mouse_vbl_q + 8'd1;
            end
            if (mouse_ab_write.assert_irq && !irqdbg_mouse_irq_prev_q &&
                irqdbg_mouse_irq_q != 8'hFF) begin
                irqdbg_mouse_irq_q <= irqdbg_mouse_irq_q + 8'd1;
            end
            if (mb1_dbg_ssi_backend_done && !irqdbg_ssi_backend_prev_q &&
                irqdbg_ssi_backend_q != 8'hFF) begin
                irqdbg_ssi_backend_q <= irqdbg_ssi_backend_q + 8'd1;
            end
            if (mb1_dbg_ssi_irq && !irqdbg_ssi_irq_prev_q &&
                irqdbg_ssi_irq_q != 8'hFF) begin
                irqdbg_ssi_irq_q <= irqdbg_ssi_irq_q + 8'd1;
            end
        end
    end

    // {mouse VBL count, mouse assert_irq count, flags, mode}
    wire [31:0] irqdbg_mouse_word = {
        irqdbg_mouse_vbl_q, irqdbg_mouse_irq_q,
        6'b0, mouse_dbg_vbl_pending, mouse_dbg_irq_pending,
        4'b0, mouse_dbg_mode
    };
    // {SSI backend-done count, SSI direct_irq count, CPU $FFFE vec count,
    //  enable_ints flag}
    wire [31:0] irqdbg_phasor_word = {
        irqdbg_ssi_backend_q, irqdbg_ssi_irq_q,
        irqdbg_vec_q, 7'b0, mb1_dbg_ssi_enable_ints
    };

    /* Mouse DEVSEL write ring (II+ ghost-write investigation): the last
     * eight writes the mouse card consumed at $C0A0-$C0AF, exactly as
     * captured (data_en-qualified). Entry = {4'b0, addr[3:0], data[7:0]},
     * newest in the low 16 bits; read via 0x31-0x34, cleared with the
     * bus forensics (write 0x2D). */
    logic [127:0] busdbg_mring_q;
    wire busdbg_mouse_wr = ab_read.data_en && !ab_read.rw &&
                           (ab_read.addr[15:4] == 12'hC0A);
    always_ff @(posedge clk) begin
        if (!rstn[2] || busdbg_clear_pulse) begin
            busdbg_mring_q <= 128'd0;
        end else if (busdbg_mouse_wr) begin
            busdbg_mring_q <= {busdbg_mring_q[111:0],
                               4'b0, ab_read.addr[3:0], ab_read.data};
        end
    end

    // Selectable slot-5 coprocessor. The PL owns the PCPI/AD8088 mailbox
    // timing and the AD8088's sparse Apple-memory cycles; either CPU runs in
    // PS software. The two personalities are mutually exclusive.
    applicard_card applicard_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_slot5_enable)),
        .card_enabled(card_slot5_enable),
        .disk2_timing_active(disk2_sound_spinning),
        .vtw_enabled(vtw_enable_eff),
        .vtw_bus_owned(vtw_bus_owned),
        .slot_assign(3'h5),
        .as_common(as_common),
        .as_client(applicard_as_client),
        .ab_write(applicard_ab_write)
    );

    uthernet2_card uthernet2_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_slot1_enable)),
        .sss(sss),
        .slot_assign(3'h1),
        .ab_write(uthernet_ab_write),
        .eth_d_i(eth_d_i),
        .eth_d_o(eth_d_o),
        .eth_d_oe(eth_d_oe),
        .eth_a(eth_a),
        .eth_rd_n(eth_rd_n),
        .eth_wr_n(eth_wr_n),
        .eth_cs_n(eth_cs_n),
        .eth_rst_n(eth_rst_n),
        .eth_int_n(eth_int_n),
        .host_req(eth_host_req_pulse),
        .host_write(eth_host_write_q),
        .host_addr(eth_host_addr_q),
        .host_wdata(eth_host_wdata_q),
        .host_ready(eth_host_ready),
        .host_done(eth_host_done),
        .host_error(eth_host_error),
        .host_rdata(eth_host_rdata)
    );

    /* Virtual Super Serial Card (printer duty). Shares slot 1 with the
     * Uthernet II: the SSC owns the slot ROM, the $C800 window, and DEVSEL
     * $C0n1/$C0n2/$C0n8-$C0nF, while the Uthernet II answers only
     * $C0n4-$C0n7. Printed bytes drain through the SSC card-control regs. */
    ssc_card ssc_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_ssc_enable)),
        .sss(sss),
        .slot_assign(3'h1),
        .ab_write(ssc_ab_write),
        .tx_count(ssc_tx_count),
        .tx_head(ssc_tx_head),
        .tx_head_valid(ssc_tx_head_valid),
        .tx_pop(ssc_tx_pop_pulse),
        .tx_clear(ssc_tx_clear_pulse),
        .tx_overflow(ssc_tx_overflow),
        .tx_overflow_clear(ssc_tx_ovf_clear_pulse),
        .acia_command(ssc_acia_command),
        .acia_control(ssc_acia_control)
    );

    // SuperSprite (TMS9918 VDP) -- PL front end for the PS software VDP.
    // Registers/VRAM/status live here; the PS renders the picture and the
    // compositor overlays it. vblank_tick reuses the boot ROM's VBL command
    // pulse (unchanged when the genlock heartbeat moved to the flywheel).
    supersprite_card supersprite_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_supersprite_enable)),
        .sss(sss),
        .slot_assign(3'h7),
        .vblank_tick(bm_vbl_cmd_pulse),
        .ps_status_flags(ss_status_flags_q),
        .ab_write(supersprite_ab_write),
        .irq_n(),                       // aggregated via ab_write.assert_irq
        .ssp_audio(ss_psg_audio),
        .ps_vram_addr(ss_vram_addr_q),
        .ps_vram_data(ss_vram_data),
        .ps_regs(ss_regs),
        .ps_status(ss_status),
        .ps_frame(ss_frame),
        .ps_status_read(),
        .ps_apple_video(ss_apple_video),
        .ps_vdp_overlay(ss_vdp_overlay)
    );

    disk2_card disk2_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(
            ab_read,
            card_slot6_enable && disk2_active_timing_q)),
        .rom_serve_en(ab_read.serve_en &&
                      card_slot6_enable && disk2_active &&
                      !disk2_active_timing_q),
        .sss(sss),
        .slot_assign(3'h6),
        .as_common(as_common),
        .as_client(disk2_as_client),
        .mc_line_addr(mc_disk2_line_addr),
        .mc_rw(mc_disk2_rw),
        .mc_wdata(mc_disk2_wdata),
        .mc_wstrb(mc_disk2_wstrb),
        .mc_valid(mc_disk2_valid),
        .mc_ready(mc_disk2_ready),
        .mc_rdata(mc_disk2_rdata),
        .mc_rvalid(mc_disk2_rvalid),
        .ab_write(disk2_ab_write),
        .vtw_active(vtw_disk2_active),
        .vtw_req_valid(vtw_d2_req_valid),
        .vtw_req_addr(vtw_d2_req_addr),
        .vtw_req_ready(vtw_d2_req_ready),
        .vtw_resp_valid(vtw_d2_resp_valid),
        .vtw_resp_rdata(vtw_d2_resp_rdata),
        .vtw_cycle_tick(vtw_d2_cycle_tick),
        .vtw_native_cycle_active(vtw_d2_native_cycle_active),
        .vtw_time_ready(vtw_d2_time_ready),
        .vtw_write_timing_active(vtw_d2_write_timing_active),
        .sound_spinning(disk2_sound_spinning),
        .sound_qtrack(disk2_sound_qtrack),
        .sound_event(disk2_sound_event),
        .sound_seek_start_qtrack(disk2_sound_seek_start_qtrack),
        .sound_seek_distance(disk2_sound_seek_distance)
    );

    wire [3:0] disk2_player_sound_event =
        (disk2_menu_sound_event_q != 4'd0) ? disk2_menu_sound_event_q : disk2_sound_event;
    wire [7:0] disk2_player_seek_start_qtrack =
        (disk2_menu_sound_event_q != 4'd0) ? 8'd0 : disk2_sound_seek_start_qtrack;
    wire [7:0] disk2_player_seek_distance = disk2_sound_seek_distance;

    disk2_sound_player disk2_sound_player_i (
        .clk(clk),
        .rstn(rstn[2]),
        .enable(disk2_sound_control_q[0]),
        .volume(disk2_sound_control_q[11:8]),
        .audio_tick(audio_sample_tick),
        .sample_base_addr(disk2_sound_sample_base_q),
        .drive_spinning(disk2_sound_spinning),
        .qtrack(disk2_sound_qtrack),
        .sound_event(disk2_player_sound_event),
        .seek_start_qtrack(disk2_player_seek_start_qtrack),
        .seek_distance(disk2_player_seek_distance),
        .sample_read(disk2_sound_read),
        .audio_l(disk2_audio_l),
        .audio_r(disk2_audio_r)
    );

    assign axi_audio_read.araddr = disk2_sound_read.araddr;
    assign axi_audio_read.arlen = disk2_sound_read.arlen;
    assign axi_audio_read.arsize = disk2_sound_read.arsize;
    assign axi_audio_read.arburst = disk2_sound_read.arburst;
    assign axi_audio_read.arvalid = disk2_sound_read.arvalid;
    assign disk2_sound_read.arready = axi_audio_read.arready;
    assign disk2_sound_read.rdata = axi_audio_read.rdata;
    assign disk2_sound_read.rresp = axi_audio_read.rresp;
    assign disk2_sound_read.rlast = axi_audio_read.rlast;
    assign disk2_sound_read.rvalid = axi_audio_read.rvalid;
    assign axi_audio_read.rready = disk2_sound_read.rready;

    /* vTW SmartPort short-circuit port (vtw_core_top <-> smartport_card). */
    wire vtw_smartport_visible =
        smartport_active && !card_supersprite_enable &&
        !vtw_disk2_boot_scan_q;
    wire         vtw_sp_active = vtw_smartport_visible;
    logic        vtw_sp_req_valid;
    logic [2:0]  vtw_sp_req_target;
    logic [10:0] vtw_sp_req_addr;
    logic        vtw_sp_req_rw;
    logic [7:0]  vtw_sp_req_wdata;
    logic        vtw_sp_req_ready;
    logic        vtw_sp_resp_valid;
    logic [7:0]  vtw_sp_resp_rdata;
    logic [21:0] vtw_sp_sss_snapshot;

    smartport_card smartport_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, vtw_smartport_visible)),
        .sss(sss),
        // SuperSprite wins the shared slot when enabled.
        .slot_assign(3'h7),
        .as_common(as_common),
        .as_client(smartport_as_client),
        .ab_write(smartport_ab_write),
        .smartport_irq(smartport_irq),
        .overlay_capture_drop(overlay_capture_drop_internal),
        .overlay_canvas_shr_active(shr_capture_active_w),
        .overlay_devsel_enabled(overlay_devsel_enabled),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(overlay_capture_bank_aux),
        .overlay_capture_base(overlay_capture_base),
        .overlay_capture_limit(overlay_capture_limit),
        .vtw_valid(vtw_sp_req_valid),
        .vtw_target(vtw_sp_req_target),
        .vtw_addr(vtw_sp_req_addr),
        .vtw_rw(vtw_sp_req_rw),
        .vtw_wdata(vtw_sp_req_wdata),
        .vtw_sss_snapshot(vtw_sp_sss_snapshot),
        .vtw_ready(vtw_sp_req_ready),
        .vtw_resp_valid(vtw_sp_resp_valid),
        .vtw_resp_rdata(vtw_sp_resp_rdata)
    );

    // PS-driven DMA command interface to apple_dma_engine.
    logic [23:0] dma_req_mc_addr;
    logic [31:0] dma_req_ddr_addr;
    logic [15:0] dma_req_length;
    logic        dma_req_rw;
    logic        dma_req_valid;
    logic        dma_req_abort;
    logic        dma_req_ready;
    logic        dma_req_done;
    logic        dma_req_abort_done;

    ps_dma_command ps_dma_command_i (
        .clk(clk),
        .rstn(rstn[3]),
        .as_common(as_common),
        .as_client(ps_dma_as_client),
        .dma_req_mc_addr(dma_req_mc_addr),
        .dma_req_ddr_addr(dma_req_ddr_addr),
        .dma_req_length(dma_req_length),
        .dma_req_rw(dma_req_rw),
        .dma_req_valid(dma_req_valid),
        .dma_req_abort(dma_req_abort),
        .dma_req_ready(dma_req_ready),
        .dma_req_done(dma_req_done),
        .dma_req_abort_done(dma_req_abort_done)
    );

    // smartport_card splits any transfer at 256-byte page boundaries
    // (and at LC-window remap edges), and the SP_STATUS payloads are
    // tiny, so req_length is bounded well below 1024 bytes. A 10-bit
    // engine shaves ~6 bits off every counter / carry chain inside
    // apple_dma_engine, which is what closes timing at 133 MHz.
    apple_dma_engine #(.LENGTH_W(10)) apple_dma_engine_i (
        .clk(clk),
        .rstn(rstn[0]),
        .req_mc_addr(dma_req_mc_addr),
        .req_ddr_addr(dma_req_ddr_addr),
        .req_length(dma_req_length[9:0]),
        .req_rw(dma_req_rw),
        .req_valid(dma_req_valid),
        .req_abort(dma_req_abort),
        .req_ready(dma_req_ready),
        .req_done(dma_req_done),
        .req_abort_done(dma_req_abort_done),
        .dma_line_addr(mc_dma_line_addr),
        .dma_rw(mc_dma_rw),
        .dma_wdata(mc_dma_wdata),
        .dma_valid(mc_dma_valid),
        .dma_ready(mc_dma_ready),
        .dma_rdata(mc_dma_rdata),
        .dma_rvalid(mc_dma_rvalid),
        .axi_hp1_read(axi_hp1_read),
        .axi_hp1_write(axi_hp1_write)
    );

    logic       bm_aux_probe_pulse;
    logic [1:0] bm_aux_status;
    logic       bm_aux_status_clear;

    boot_menu_card boot_menu_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(ab_read),
        .sss(sss),
        .disk2_enabled(card_slot6_enable),
        .apple_video_mode_valid(video_mode_50hz_valid_q),
        .apple_video_mode_50hz(video_mode_50hz),
        .as_common(as_common),
        .as_client(boot_menu_as_client),
        .ab_write(boot_menu_ab_write),
        .smartport_active(smartport_active),
        .disk2_active(disk2_active),
        .boot_target_disk2(boot_target_disk2),
        .boot_slot(boot_menu_slot),
        .boot_slot_valid(boot_menu_slot_valid),
        .apple_vblank_start_pulse(bm_vbl_cmd_pulse),
        .aux_probe_pulse(bm_aux_probe_pulse),
        .aux_status(bm_aux_status),
        .aux_status_clear(bm_aux_status_clear)
    );

    no_slot_clock no_slot_clock_i (
        .clk(clk),
        .rstn(rstn[2]),
        .enabled(no_slot_clock_enabled),
        .slot_mask(no_slot_clock_slot_mask),
        .time_bcd(nsc_time_bcd_q),
        .ab_read(ab_read),
        .sss(sss),
        .write_time_bcd(nsc_write_time_bcd),
        .write_time_strobe(nsc_write_time_strobe),
        .ab_write(no_slot_clock_ab_write)
    );

    // ------------------------------------------------------------------
    // Virtual TransWarp accelerator. Bus-master client of the arbiter;
    // supported on positively identified //e and II/II+ hosts. The accepted
    // machine verdict is latched for the session so CTRL-RESET cannot drop
    // DMA ownership while the motherboard CPU is stopped.
    // ------------------------------------------------------------------
    globals::AppleBus_write vtw_ab_write;
    logic [31:0] vtw_ctrl_q;
    logic [31:0] vtw_slowdown_q;   // [9:0] region enables, [31:16] duration
    logic [17:0] vtw_sh_addr_q;
    logic [7:0]  vtw_sh_rdata;
    logic [7:0]  vtw_sh_rdata_q;
    logic        vtw_sh_port_en;
    logic        vtw_sh_port_we;
    logic [7:0]  vtw_sh_port_wdata;
    logic        vtw_sh_word_ready;
    logic        vtw_sh_word_busy;
    logic [29:0] vtw_sh_word_accept_count;
    logic [31:0] vtw_sh_word_read_data;
    logic        vtw_sh_word_read_ready;
    logic        vtw_sh_word_read_busy;
    logic [29:0] vtw_sh_word_read_count;
    logic        vtw_arm_go_pulse_q;
    logic [15:0] vtw_arm_addr_q;
    logic        vtw_arm_rw_q;
    logic [7:0]  vtw_arm_wdata_q;
    logic        vtw_arm_busy;
    logic        vtw_arm_resp_valid;
    logic [7:0]  vtw_arm_rdata;
    logic        vtw_sync_done_q;
    logic        vtw_arm_post_pulse_q;
    logic [15:0] vtw_arm_post_addr_q;
    logic [7:0]  vtw_arm_post_wdata_q;
    logic        vtw_arm_post_ready;
    logic [30:0] vtw_arm_post_accept_count_q;
    logic        vtw_arm_rw_flush_pulse_q;
    logic        vtw_arm_rw_release_pulse_q;
    logic        vtw_arm_rw_flush_done;
    logic        vtw_arm_rw_hold_state;
    logic        vtw_arm_rw_flush_busy_q;
    logic [29:0] vtw_arm_rw_flush_count_q;
    logic [1:0]  vtw_c074_state;
    logic [15:0] vtw_dbg_core_pc;
    logic [31:0] vtw_cnt_core_cycles;
    logic [31:0] vtw_cnt_bus_cycles;
    logic [31:0] vtw_cnt_posted_writes;
    logic [9:0]  vtw_post_fill;
    logic [9:0]  vtw_post_high_water;
    logic [31:0] vtw_cnt_post_drops;
    logic [31:0] vtw_cnt_invalid_routes;
    logic [10:0] vtw_dbg_vsss;
    logic [15:0] vtw_dbg_last_sync_addr;
    logic [7:0]  vtw_dbg_last_sync_data;
    logic        vtw_dbg_last_sync_rw;
    logic [6:0]  vtw_dbg_irq_edges;
    logic [127:0] vtw_dbg_cxxx_ring;
    logic [127:0] vtw_dbg_c0_ring;
    logic [31:0] vtw_dbg_sync_write_check;
    logic [15:0] vtw_dbg_sync_write_addr;
    logic [31:0] vtw_dbg_c000_context;
    logic [31:0] vtw_dbg_c000_counts;
    logic [16*16-1:0] vtw_dbg_pc_trace;
    logic [16*32-1:0] vtw_dbg_io_trace;
    logic [31:0] vtw_dbg_trace_status;
    logic [31:0] vtw_dbg_bus_faults;

    wire vtw_sh_addr_set = as_client.awvalid &&
                           (as_common.awaddr == CARD_CTRL_REG_VTW_SHADOW_ADDR) &&
                           (as_vtw_phasor_wstrb != 4'b0000);
    wire vtw_sh_byte_write = as_client.awvalid &&
                             (as_common.awaddr == CARD_CTRL_REG_VTW_SHADOW_DATA) &&
                             as_vtw_phasor_wstrb[0];
    wire vtw_sh_word_write = as_client.awvalid &&
                             (as_common.awaddr == CARD_CTRL_REG_VTW_SHADOW_DATA4) &&
                             (as_vtw_phasor_wstrb == 4'b1111);
    wire vtw_sh_word_read = as_client.awvalid &&
                            (as_common.awaddr == CARD_CTRL_REG_VTW_SHADOW_READ4) &&
                            as_vtw_phasor_wstrb[0] && as_vtw_phasor_wdata[0];

    /* Machine gate, latched for the lifetime of the enable request. The
     * PS re-identifies the machine on every Apple reset (machine_mode
     * passes through UNKNOWN until the boot ROM re-reports), but the
     * session must survive CTRL-RESET with /DMA held (README §2) -- a
     * dropped gate would wake the insane motherboard 6502 mid-session.
     * A machine cannot change type while powered; the verdict only
     * matters at session start. Both //e (mode 2) and II/II+ (mode 1)
     * accelerate as an Enhanced //e: the fixed //e ROM and the core's full
     * MMU model apply regardless of host. */
    logic vtw_machine_ok_q;
    logic vtw_host_is_iiplus_q;
    always_ff @(posedge clk) begin
        if (!rstn[1]) begin
            vtw_machine_ok_q       <= 1'b0;
            vtw_host_is_iiplus_q   <= 1'b0;
        end
        else if (!vtw_ctrl_q[0]) begin
            vtw_machine_ok_q       <= 1'b0;
            vtw_host_is_iiplus_q   <= 1'b0;
        end
        else if (!vtw_machine_ok_q && !physical_bus_isolate) begin
            if (machine_mode_q == 2'd1) begin
                vtw_machine_ok_q       <= 1'b1;
                vtw_host_is_iiplus_q   <= 1'b1;
            end
            else if (machine_mode_q == 2'd2) begin
                vtw_machine_ok_q       <= 1'b1;
                vtw_host_is_iiplus_q   <= 1'b0;
            end
        end
    end
    /* Every II/II+ vTW session refreshes the weak translated /DMA hold. Use
     * the session-latched verdict so refresh remains active while Apple RESET
     * temporarily makes the live machine report UNKNOWN. A //e can never
     * activate this path. */
    assign vtw_iiplus_dma_refresh_active =
        !physical_bus_isolate && vtw_host_is_iiplus_q;
    // Raw vertical-blank level for the core's synthesized $C019 read. The
    // vTW applies its CPU-visible one-cycle lag locally from line/cycle, so
    // native timing consumers remain on the calibrated 192:0 boundary.
    // Same fabric clock as the timing generator, so no synchronizer is needed.
    wire vtw_video_vbl = (line_in_frame >= 9'd192);
    // ONE//e is always an Enhanced //e and needs no physical-machine verdict.
    // During the request-to-effective guard interval, and after an activity
    // kill while isolation remains held, neither virtual nor physical vTW may
    // run.
    assign vtw_enable_eff = vtw_ctrl_q[0] &&
        (onee_enable_effective ||
         (!physical_bus_isolate && vtw_machine_ok_q));
    wire vtw_host_is_iiplus_eff =
        !onee_enable_effective && vtw_host_is_iiplus_q;
    wire vtw_core_run_eff = vtw_enable_eff && vtw_ctrl_q[1];

    // The first slot-ROM handoff read needs the live gate. Normal Disk II I/O,
    // timing, and vTW can take it one fabric clock later, cutting the slot-7
    // decode from the controller's rotation and stepper enables.
    always_ff @(posedge clk) begin
        if (!rstn[1])
            disk2_active_timing_q <= 1'b0;
        else
            disk2_active_timing_q <= disk2_active;
    end

    assign vtw_disk2_active = vtw_core_run_eff && vtw_bus_owned &&
                              card_slot6_enable && disk2_active_timing_q &&
                              !vtw_ctrl_q[7];
    wire vtw_slot6_boot_probe =
        vtw_disk2_boot_scan_q && vtw_bus_owned &&
        ab_read.serve_en && ab_read.rw &&
        (ab_read.addr[15:8] == 8'hC6);

    /* The native boot-menu ROM has already handed off to the selected
     * device before vTW takes the bus. vTW then resets the machine and
     * starts the accelerated core from a fresh //e ROM image, creating a
     * second cold slot scan. If Disk II was selected, hide slot 7 only for
     * that scan so the //e ROM reaches slot 6 instead of rediscovering the
     * normally active SmartPort first. Restore slot 7 immediately when the
     * scan probes slot 6; SmartPort remains available after boot. */
    always_ff @(posedge clk) begin
        if (!rstn[1]) begin
            vtw_core_run_eff_q    <= 1'b0;
            vtw_disk2_boot_scan_q <= 1'b0;
        end else begin
            vtw_core_run_eff_q <= vtw_core_run_eff;
            if (!vtw_core_run_eff) begin
                vtw_disk2_boot_scan_q <= 1'b0;
            end else if (!vtw_core_run_eff_q) begin
                vtw_disk2_boot_scan_q <= boot_target_disk2;
            end else if (vtw_slot6_boot_probe) begin
                vtw_disk2_boot_scan_q <= 1'b0;
            end
        end
    end

    /* SHR paged-mode posting fallback (CARD_CTRL 0x35 bit 0). */
    logic post_main_wide_q;

    vtw_shadow_host_port vtw_shadow_host_port_i (
        .clk(clk),
        .rstn(rstn[3]),
        .addr_set(vtw_sh_addr_set),
        .addr_value(as_vtw_phasor_wdata[17:0]),
        .byte_write(vtw_sh_byte_write),
        .byte_wdata(as_vtw_phasor_wdata[7:0]),
        .word_write(vtw_sh_word_write),
        .word_wdata(as_vtw_phasor_wdata),
        .word_read(vtw_sh_word_read),
        .pointer(vtw_sh_addr_q),
        .read_data(vtw_sh_rdata_q),
        .word_ready(vtw_sh_word_ready),
        .word_busy(vtw_sh_word_busy),
        .word_accept_count(vtw_sh_word_accept_count),
        .word_read_data(vtw_sh_word_read_data),
        .word_read_ready(vtw_sh_word_read_ready),
        .word_read_busy(vtw_sh_word_read_busy),
        .word_read_count(vtw_sh_word_read_count),
        .sh_en(vtw_sh_port_en),
        .sh_addr(),
        .sh_we(vtw_sh_port_we),
        .sh_wdata(vtw_sh_port_wdata),
        .sh_rdata(vtw_sh_rdata)
    );

    vtw_core_top vtw_core_top_i (
        .clk(clk),
        .rstn(rstn[1]),
        .enable(vtw_enable_eff),
        .host_is_iiplus(vtw_host_is_iiplus_eff),
        .core_run(vtw_ctrl_q[1]),
        .assert_apple_res(vtw_ctrl_q[4]),
        .speed_mode(vtw_ctrl_q[3:2]),
        .pace_divider(vtw_ctrl_q[31:16]),
        .ignore_c074(vtw_ctrl_q[6]),
        .irq_assert_in(ab_write_arb.assert_irq),
        .data_drive_in(ab_write_arb.wr_data_en),
        .data_drive_value_in(ab_write_arb.wr_data),
        .dbg_clear(busdbg_clear_pulse),
        .iiplus_buttons_zero(vtw_ctrl_q[5]),
        .slow_region_en(vtw_slowdown_q[9:0]),
        .slow_duration(vtw_slowdown_q[31:16]),
        .d2_active(vtw_disk2_active),
        .d2_req_valid(vtw_d2_req_valid),
        .d2_req_addr(vtw_d2_req_addr),
        .d2_req_ready(vtw_d2_req_ready),
        .d2_resp_valid(vtw_d2_resp_valid),
        .d2_resp_rdata(vtw_d2_resp_rdata),
        .d2_cycle_tick(vtw_d2_cycle_tick),
        .d2_native_cycle_active(vtw_d2_native_cycle_active),
        .d2_time_ready(vtw_d2_time_ready),
        .d2_write_timing_active(vtw_d2_write_timing_active),
        .ramworks_en(ramworks_en_q),
        .video_vbl(vtw_video_vbl),
        .post_main_wide(post_main_wide_q),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(overlay_capture_bank_aux),
        .overlay_capture_base(overlay_capture_base),
        .overlay_capture_limit(overlay_capture_limit),
        .video_mode_50hz(video_mode_50hz),
        .video_line(line_in_frame),
        .video_cycle(cycle_in_line),
        .ab_read(ab_read),
        .ab_write(vtw_ab_write),
        .rw_req_valid(vtw_rw_req_valid),
        .rw_req_rw(vtw_rw_req_rw),
        .rw_req_addr(vtw_rw_req_addr),
        .rw_req_wline(vtw_rw_req_wline),
        .rw_req_ready(vtw_rw_req_ready),
        .rw_resp_valid(vtw_rw_resp_valid),
        .rw_resp_rline(vtw_rw_resp_rline),
        .sp_active(vtw_sp_active),
        .sp_boot_suppress(vtw_disk2_boot_scan_q),
        .sp_req_valid(vtw_sp_req_valid),
        .sp_req_target(vtw_sp_req_target),
        .sp_req_addr(vtw_sp_req_addr),
        .sp_req_rw(vtw_sp_req_rw),
        .sp_req_wdata(vtw_sp_req_wdata),
        .sp_req_ready(vtw_sp_req_ready),
        .sp_resp_valid(vtw_sp_resp_valid),
        .sp_resp_rdata(vtw_sp_resp_rdata),
        .sp_sss_snapshot(vtw_sp_sss_snapshot),
        .sh_en(vtw_sh_port_en),
        .sh_addr(vtw_sh_addr_q),
        .sh_we(vtw_sh_port_we),
        .sh_wdata(vtw_sh_port_wdata),
        .sh_rdata(vtw_sh_rdata),
        .arm_req_valid(vtw_arm_go_pulse_q),
        .arm_req_addr(vtw_arm_addr_q),
        .arm_req_rw(vtw_arm_rw_q),
        .arm_req_wdata(vtw_arm_wdata_q),
        .arm_req_busy(vtw_arm_busy),
        .arm_resp_valid(vtw_arm_resp_valid),
        .arm_resp_rdata(vtw_arm_rdata),
        .arm_post_we(vtw_arm_post_pulse_q),
        .arm_post_addr(vtw_arm_post_addr_q),
        .arm_post_wdata(vtw_arm_post_wdata_q),
        .arm_post_ready(vtw_arm_post_ready),
        .arm_rw_flush_req(vtw_arm_rw_flush_pulse_q),
        .arm_rw_hold_release(vtw_arm_rw_release_pulse_q),
        .arm_rw_flush_done(vtw_arm_rw_flush_done),
        .arm_rw_hold_state(vtw_arm_rw_hold_state),
        .c074_state(vtw_c074_state),
        .bus_owned(vtw_bus_owned),
        .video_phase_1mhz(vtw_video_phase_1mhz),
        .dbg_core_pc(vtw_dbg_core_pc),
        .cnt_core_cycles(vtw_cnt_core_cycles),
        .cnt_bus_cycles(vtw_cnt_bus_cycles),
        .cnt_posted_writes(vtw_cnt_posted_writes),
        .post_fill(vtw_post_fill),
        .post_high_water(vtw_post_high_water),
        .cnt_post_drops(vtw_cnt_post_drops),
        .cnt_invalid_routes(vtw_cnt_invalid_routes),
        .dbg_vsss(vtw_dbg_vsss),
        .dbg_last_sync_addr(vtw_dbg_last_sync_addr),
        .dbg_last_sync_data(vtw_dbg_last_sync_data),
        .dbg_last_sync_rw(vtw_dbg_last_sync_rw),
        .dbg_irq_edges(vtw_dbg_irq_edges),
        .dbg_cxxx_ring(vtw_dbg_cxxx_ring),
        .dbg_c0_ring(vtw_dbg_c0_ring),
        .dbg_sync_write_check(vtw_dbg_sync_write_check),
        .dbg_sync_write_addr(vtw_dbg_sync_write_addr),
        .dbg_c000_context(vtw_dbg_c000_context),
        .dbg_c000_counts(vtw_dbg_c000_counts),
        .dbg_pc_trace(vtw_dbg_pc_trace),
        .dbg_io_trace(vtw_dbg_io_trace),
        .dbg_trace_status(vtw_dbg_trace_status),
        .dbg_bus_faults(vtw_dbg_bus_faults)
    );

    // apple_bus_write_arbiter merges virtual-card responses and control-line
    // requests before they reach apple_bus_wrapper. The vTW is prepended so
    // every existing client keeps its index. vTW and AD8088 can both drive
    // address/RW, but applicard_card blocks AD8088 whenever vTW is enabled,
    // so the two bus masters cannot contend at this priority mux.
    apple_bus_write_arbiter #(
        .NUM_CLIENTS(12),
        .FAST_DATA_CLIENT(2),
        .FAST_ADDR_CLIENT(11)
    )
    apple_bus_write_arbiter_i(
        .inh_allowed(machine_inh_allowed),
        .client_writes({
            vtw_ab_write,
            brain_transplant_write,
            mb1_ab_write,
            mouse_ab_write,
            applicard_ab_write,
            uthernet_ab_write,
            ssc_ab_write,
            supersprite_ab_write,
            disk2_ab_write,
            smartport_ab_write,
            boot_menu_ab_write,
            no_slot_clock_ab_write
        }),
        .ab_write(ab_write_arb)
    );

    // AxiSimple card-control and diagnostic register mux.
    //   araddr 0x13: aux/RamWorks reads
    //   araddr 0x14: aux/RamWorks read deadline misses
    //   araddr 0x15: PS DMA requests admitted
    //   araddr 0x16: PS DMA requests completed
    //   araddr 0x17: aux/RamWorks writes
    //   araddr 0x18: late PSRAM serves
    //   araddr 0x1D: maximum PSRAM serve latency
    // Card control registers:
    //   araddr/awaddr 0x00: slot enable mask, bits 1..7 map to Apple slots
    //   araddr/awaddr 0x01: feature enable mask
    //                         bit 0 = DS1216E no-slot clock under this ROM
    //   araddr 0x02: current soft-switch snapshot from soft_switch_manager
    //   awaddr 0x03: reset release latch:
    //                   bit 0 = CPU0/frontend ready
    //                   bit 1 = CPU1/renderer ready
    //                   Apple RESET# releases when both are set
    //   araddr/awaddr 0x04: no-slot clock BCD time bits 31..0
    //   araddr/awaddr 0x05: no-slot clock BCD time bits 63..32
    //                         write low first, then high; high commits.
    //   araddr 0x06: no-slot clock Apple-write sequence counter
    //   awaddr 0x07: menu chime trigger, bit 0 starts the fixed PL chime.
    //   araddr/awaddr 0x08: Phasor pan low, AY0/AY1 six 4-bit values.
    //   araddr 0x09: Apple reset status, bits 7..0 reset assert sequence,
    //                 bit 8 = sampled Apple RES# level.
    //   araddr/awaddr 0x0A: Phasor pan high, AY2/AY3 six 4-bit values.
    //   araddr/awaddr 0x0C: Phasor audio controls, signed 5-bit fields:
    //                         bass, mid, treble, warmth, volume;
    //                         bit 25 selects PSG volume table (0=YM, 1=AY).
    //   araddr/awaddr 0x10: Disk II mechanical sound sample table base address.
    //   araddr/awaddr 0x11: Disk II mechanical sound control, bit 0 enables,
    //                         [11:8] volume, write-only [19:16] menu event.
    //
    // rdata MUST be registered (not always_comb): the axidouble crossbar's
    // addrdecode is OPT_REGISTERED=1 without OPT_LOWPOWER, so it advances
    // o_addr to next_araddr the cycle after a read fires. axidouble samples
    // M_AXI_RDATA in that already-advanced cycle, so a combinational slave
    // returns the next register's value (off-by-one shift). Registering the
    // mux adds the matching one-cycle latency.
    logic [31:0] as_client_rdata_q;
    always_ff @(posedge clk) begin
        if (!rstn[3]) begin
            card_slot_enable_mask_q <= card_slot_enable_normalize(CARD_CTRL_SLOT_ENABLE_RESET);
            card_feature_enable_mask_q <= 32'h0000_0000;
            reset_release_ready_q <= 32'h0000_0000;
            menu_chime_start_q <= 1'b0;
            phasor_pan_q <= PHASOR_PAN_RESET;
            phasor_audio_q <= PHASOR_AUDIO_RESET;
            eth_host_addr_q <= 16'h0000;
            eth_host_wdata_q <= 8'h00;
            eth_host_rdata_q <= 8'h00;
            eth_host_req_pulse <= 1'b0;
            eth_host_write_q <= 1'b0;
            eth_host_busy_q <= 1'b0;
            eth_host_done_q <= 1'b0;
            eth_host_error_q <= 1'b0;
            disk2_sound_sample_base_q <= 32'h0000_0000;
            disk2_sound_control_q <= 32'h0000_0000;
            disk2_menu_sound_event_q <= 4'd0;
            nsc_time_shadow_q <= NSC_TIME_RESET;
            nsc_time_bcd_q <= NSC_TIME_RESET;
            nsc_write_seq_q <= 32'h0000_0000;
            apple_reset_seq_q <= 8'h00;
            ss_vram_addr_q <= 14'd0;
            ss_status_flags_q <= 7'd0;
            as_client_rdata_q <= 32'h0000_0000;
            egress_cfg_enable_q             <= 1'b0;
            egress_cfg_ring_base_q          <= 32'h0;
            egress_cfg_ring_size_log2_q     <= 5'd19; // 512 KB default
            egress_cfg_producer_ptr_addr_q  <= 32'h0;
            egress_cfg_consumer_ptr_q       <= 32'h0;
            egress_cfg_reset_pulse          <= 1'b0;
            sdd_cfg_enable_q                <= 1'b0;
            sdd_cfg_ring_base_q             <= 32'h0;
            sdd_cfg_ring_size_log2_q        <= 5'd17; // 128 KB default
            sdd_cfg_producer_ptr_addr_q     <= 32'h0;
            sdd_cfg_consumer_ptr_q          <= 32'h0;
            sdd_cfg_reset_pulse             <= 1'b0;
            onee_request_q                  <= 1'b0;
            machine_mode_q                  <= 2'd0;
            machine_inh_allowed_wrapper_q   <= 1'b0;
            machine_m2sel_active_high       <= 1'b0;
            aux_provide_en_q                <= 1'b0;
            psram_dcount_q                  <= 5'd0;
            ramworks_en_q                   <= 1'b0;
            post_main_wide_q                <= 1'b0;
            psram_dcount_edge_q             <= 1'b0;
            psram_dcount_wr_pulse_q         <= 1'b0;
            vtw_ctrl_q                      <= 32'h0000_0000;
            vtw_slowdown_q                  <= 32'h0000_0000;
            vtw_arm_go_pulse_q              <= 1'b0;
            vtw_arm_addr_q                  <= 16'h0000;
            vtw_arm_rw_q                    <= 1'b1;
            vtw_arm_wdata_q                 <= 8'h00;
            vtw_sync_done_q                 <= 1'b0;
            vtw_arm_post_pulse_q            <= 1'b0;
            vtw_arm_post_addr_q             <= 16'h0000;
            vtw_arm_post_wdata_q            <= 8'h00;
            vtw_arm_post_accept_count_q     <= 31'd0;
            vtw_arm_rw_flush_pulse_q        <= 1'b0;
            vtw_arm_rw_release_pulse_q      <= 1'b0;
            vtw_arm_rw_flush_busy_q         <= 1'b0;
            vtw_arm_rw_flush_count_q        <= 30'd0;
        end else begin
            // cfg_reset_pulse is a one-cycle pulse: cleared every cycle and
            // set only when an awvalid lands on 8'h25.
            egress_cfg_reset_pulse <= 1'b0;
            sdd_cfg_reset_pulse <= 1'b0;
            psram_dcount_wr_pulse_q <= 1'b0;
            forensics_clear_pulse <= 1'b0;
            busdbg_clear_pulse <= 1'b0;
            bm_aux_status_clear <= 1'b0;
            menu_chime_start_q <= 1'b0;
            disk2_menu_sound_event_q <= 4'd0;
            eth_host_req_pulse <= 1'b0;
            ssc_tx_pop_pulse <= 1'b0;
            ssc_tx_clear_pulse <= 1'b0;
            ssc_tx_ovf_clear_pulse <= 1'b0;
            vtw_arm_go_pulse_q <= 1'b0;
            vtw_arm_post_pulse_q <= 1'b0;
            vtw_arm_rw_flush_pulse_q <= 1'b0;
            vtw_arm_rw_release_pulse_q <= 1'b0;

            if (vtw_arm_post_pulse_q && vtw_arm_post_ready) begin
                vtw_arm_post_accept_count_q <=
                    vtw_arm_post_accept_count_q + 31'd1;
            end
            if (vtw_arm_rw_flush_done) begin
                vtw_arm_rw_flush_busy_q  <= 1'b0;
                vtw_arm_rw_flush_count_q <= vtw_arm_rw_flush_count_q + 30'd1;
            end

            if (vtw_arm_resp_valid) begin
                vtw_sync_done_q <= 1'b1;
            end

            if (eth_host_done) begin
                eth_host_rdata_q <= eth_host_rdata;
                eth_host_busy_q <= 1'b0;
                eth_host_done_q <= 1'b1;
                eth_host_error_q <= eth_host_error_q | eth_host_error;
            end

            if (apple_reset_assert_pulse) begin
                apple_reset_seq_q <= apple_reset_seq_q + 8'd1;
            end

            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    CARD_CTRL_REG_SLOT_ENABLE_MASK: begin
                        card_slot_enable_mask_q <= card_slot_enable_normalize(
                            globals::apply_wstrb(
                                card_slot_enable_mask_q,
                                as_common.wdata,
                                as_common.wstrb
                            )
                        );
                    end
                    CARD_CTRL_REG_FEATURE_ENABLE_MASK: begin
                        card_feature_enable_mask_q <= globals::apply_wstrb(
                            card_feature_enable_mask_q,
                            as_common.wdata,
                            as_common.wstrb
                        );
                    end
                    CARD_CTRL_REG_RESET_RELEASE: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            32'h0000_0000, as_common.wdata, as_common.wstrb);
                        reset_release_ready_q <=
                            reset_release_ready_q | (tmp & RESET_RELEASE_READY_MASK);
                    end
                    CARD_CTRL_REG_NSC_TIME_LO: begin
                        nsc_time_shadow_q[31:0] <= globals::apply_wstrb(
                            nsc_time_shadow_q[31:0],
                            as_common.wdata,
                            as_common.wstrb
                        );
                    end
                    CARD_CTRL_REG_NSC_TIME_HI: begin
                        automatic logic [31:0] nsc_hi = globals::apply_wstrb(
                            nsc_time_shadow_q[63:32],
                            as_common.wdata,
                            as_common.wstrb
                        );
                        nsc_time_shadow_q[63:32] <= nsc_hi;
                        nsc_time_bcd_q <= {nsc_hi, nsc_time_shadow_q[31:0]};
                    end
                    CARD_CTRL_REG_MENU_CHIME: begin
                        menu_chime_start_q <= as_common.wstrb[0] && as_common.wdata[0];
                    end
                    CARD_CTRL_REG_PHASOR_PAN_LO: begin
                        automatic logic [31:0] pan_tmp = globals::apply_wstrb(
                            {8'h00, phasor_pan_q[23:0]},
                            as_vtw_phasor_wdata, as_vtw_phasor_wstrb);
                        phasor_pan_q[23:0] <= pan_tmp[23:0];
                    end
                    CARD_CTRL_REG_PHASOR_PAN_HI: begin
                        automatic logic [31:0] pan_tmp = globals::apply_wstrb(
                            {8'h00, phasor_pan_q[47:24]},
                            as_vtw_phasor_wdata, as_vtw_phasor_wstrb);
                        phasor_pan_q[47:24] <= pan_tmp[23:0];
                    end
                    CARD_CTRL_REG_PHASOR_AUDIO: begin
                        phasor_audio_q <= globals::apply_wstrb(
                            phasor_audio_q, as_vtw_phasor_wdata, as_vtw_phasor_wstrb);
                    end
                    CARD_CTRL_REG_ETH_ADDR: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {16'h0000, eth_host_addr_q},
                            as_common.wdata,
                            as_common.wstrb);
                        eth_host_addr_q <= tmp[15:0];
                    end
                    CARD_CTRL_REG_ETH_DATA: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {24'h000000, eth_host_wdata_q},
                            as_common.wdata,
                            as_common.wstrb);
                        eth_host_wdata_q <= tmp[7:0];
                    end
                    CARD_CTRL_REG_ETH_CMD: begin
                        if ((as_common.wstrb[0] != 1'b0) &&
                            ((as_common.wdata & CARD_CTRL_ETH_CMD_GO) != 32'h0000_0000)) begin
                            /* The card latches the request even when it
                             * cannot start this cycle (pending latch),
                             * so done always arrives; only a GO while a
                             * previous op is still in flight is an
                             * error. */
                            if (!eth_host_busy_q) begin
                                eth_host_write_q <=
                                    ((as_common.wdata & CARD_CTRL_ETH_CMD_WRITE) !=
                                     32'h0000_0000);
                                eth_host_done_q <= 1'b0;
                                eth_host_error_q <= 1'b0;
                                eth_host_req_pulse <= 1'b1;
                                eth_host_busy_q <= 1'b1;
                            end else begin
                                eth_host_done_q <= 1'b1;
                                eth_host_error_q <= 1'b1;
                            end
                        end
                    end
                    CARD_CTRL_REG_SSC_CTRL: begin
                        if (as_common.wstrb[0] != 1'b0) begin
                            ssc_tx_pop_pulse       <= as_common.wdata[0];
                            ssc_tx_clear_pulse     <= as_common.wdata[1];
                            ssc_tx_ovf_clear_pulse <= as_common.wdata[2];
                        end
                    end
                    CARD_CTRL_REG_DISK2_SOUND_BASE: begin
                        disk2_sound_sample_base_q <= globals::apply_wstrb(
                            disk2_sound_sample_base_q, as_common.wdata, as_common.wstrb);
                    end
                    CARD_CTRL_REG_DISK2_SOUND_CONTROL: begin
                        automatic logic [31:0] disk2_sound_control_tmp =
                            globals::apply_wstrb(
                                disk2_sound_control_q,
                                as_common.wdata,
                                as_common.wstrb);
                        disk2_sound_control_q <=
                            disk2_sound_control_tmp & ~CARD_CTRL_DISK2_SOUND_EVENT_MASK;
                        if (as_common.wstrb[CARD_CTRL_DISK2_SOUND_EVENT_SHIFT / 8])
                            disk2_menu_sound_event_q <=
                                disk2_sound_control_tmp[
                                    CARD_CTRL_DISK2_SOUND_EVENT_SHIFT +: 4];
                    end
                    8'h20: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {31'b0, egress_cfg_enable_q}, as_common.wdata, as_common.wstrb);
                        egress_cfg_enable_q <= tmp[0];
                    end
                    8'h21: egress_cfg_ring_base_q <= globals::apply_wstrb(
                                egress_cfg_ring_base_q, as_common.wdata, as_common.wstrb);
                    8'h22: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {27'b0, egress_cfg_ring_size_log2_q},
                            as_common.wdata, as_common.wstrb);
                        egress_cfg_ring_size_log2_q <= tmp[4:0];
                    end
                    8'h23: egress_cfg_producer_ptr_addr_q <= globals::apply_wstrb(
                                egress_cfg_producer_ptr_addr_q, as_common.wdata, as_common.wstrb);
                    8'h24: egress_cfg_consumer_ptr_q <= globals::apply_wstrb(
                                egress_cfg_consumer_ptr_q, as_common.wdata, as_common.wstrb);
                    8'h25: egress_cfg_reset_pulse <= as_common.wdata[0];
                    8'h50: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {31'b0, sdd_cfg_enable_q}, as_common.wdata, as_common.wstrb);
                        sdd_cfg_enable_q <= tmp[0];
                    end
                    8'h51: sdd_cfg_ring_base_q <= globals::apply_wstrb(
                                sdd_cfg_ring_base_q, as_common.wdata, as_common.wstrb);
                    8'h52: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {27'b0, sdd_cfg_ring_size_log2_q},
                            as_common.wdata, as_common.wstrb);
                        sdd_cfg_ring_size_log2_q <= tmp[4:0];
                    end
                    8'h53: sdd_cfg_producer_ptr_addr_q <= globals::apply_wstrb(
                                sdd_cfg_producer_ptr_addr_q, as_common.wdata, as_common.wstrb);
                    8'h54: sdd_cfg_consumer_ptr_q <= globals::apply_wstrb(
                                 sdd_cfg_consumer_ptr_q, as_common.wdata, as_common.wstrb);
                    8'h55: sdd_cfg_reset_pulse <= as_common.wdata[0];
                    CARD_CTRL_REG_ONEE: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {31'b0, onee_request_q},
                            as_common.wdata, as_common.wstrb);
                        onee_request_q <= tmp[0];
                    end
                    8'h60: begin
                        machine_mode_q            <= as_common.wdata[1:0];
                        machine_inh_allowed_wrapper_q <=
                            (as_common.wdata[1:0] == 2'd1) ||
                            (as_common.wdata[1:0] == 2'd2);
                        machine_m2sel_active_high <= as_common.wdata[2];
                    end
                    8'h61: aux_provide_en_q <= as_common.wdata[0];
                    /* (aux_provide is also force-dropped by the boot
                     * ROM's aux probe -- see bm_aux_probe_pulse term
                     * below this case block.) */
                    8'h62: ramworks_en_q <= as_common.wdata[0];
                    8'h63: begin
                        psram_dcount_q          <= as_common.wdata[4:0];
                        psram_dcount_edge_q     <= as_common.wdata[5];
                        psram_dcount_wr_pulse_q <= 1'b1;
                    end
                    8'h64: forensics_clear_pulse <= 1'b1;
                    /* Bus-capture forensics: any write clears the wrapper
                     * counters and the mouse DEVSEL write ring. */
                    8'h2D: busdbg_clear_pulse <= 1'b1;
                    8'h35: post_main_wide_q <= as_common.wdata[0];
                    8'h6A: bm_aux_status_clear <= 1'b1;
                    CARD_CTRL_REG_SS_VRAM_ADDR: begin
                        automatic logic [31:0] a = globals::apply_wstrb(
                            {18'b0, ss_vram_addr_q}, as_common.wdata, as_common.wstrb);
                        ss_vram_addr_q <= a[13:0];
                    end
                    CARD_CTRL_REG_SS_SPR_FLAGS: begin
                        automatic logic [31:0] f = globals::apply_wstrb(
                            {25'b0, ss_status_flags_q}, as_common.wdata, as_common.wstrb);
                        ss_status_flags_q <= f[6:0];
                    end
                    CARD_CTRL_REG_VTW_CTRL: begin
                        vtw_ctrl_q <= globals::apply_wstrb(
                            vtw_ctrl_q, as_common.wdata, as_common.wstrb);
                    end
                    CARD_CTRL_REG_VTW_SLOWDOWN: begin
                        vtw_slowdown_q <= globals::apply_wstrb(
                            vtw_slowdown_q, as_common.wdata, as_common.wstrb);
                    end
                    CARD_CTRL_REG_VTW_SYNC_CMD: begin
                        vtw_arm_addr_q     <= as_common.wdata[15:0];
                        vtw_arm_wdata_q    <= as_common.wdata[23:16];
                        vtw_arm_rw_q       <= as_common.wdata[24];
                        vtw_arm_go_pulse_q <= 1'b1;
                        vtw_sync_done_q    <= 1'b0;
                    end
                    CARD_CTRL_REG_VTW_POST_PUSH: begin
                        vtw_arm_post_addr_q  <= as_common.wdata[15:0];
                        vtw_arm_post_wdata_q <= as_common.wdata[23:16];
                        vtw_arm_post_pulse_q <= 1'b1;
                    end
                    CARD_CTRL_REG_VTW_RW_FLUSH: begin
                        if (as_common.wdata[0] && !vtw_arm_rw_flush_busy_q) begin
                            vtw_arm_rw_flush_pulse_q <= 1'b1;
                            vtw_arm_rw_flush_busy_q  <= 1'b1;
                        end
                        if (as_common.wdata[1]) begin
                            vtw_arm_rw_release_pulse_q <= 1'b1;
                        end
                    end
                    default: begin
                    end
                endcase
            end

            // Raw or sticky Apple activity has priority over a same-cycle
            // software write. Clearing the request also makes a new off/on
            // selection mandatory after the guard's quiet interval.
            if (onee_activity_now || onee_activity_lockout) begin
                onee_request_q <= 1'b0;
            end

            if (bm_aux_probe_pulse) begin
                aux_provide_en_q <= 1'b0;
            end

            if (nsc_write_time_strobe) begin
                nsc_time_shadow_q <= nsc_write_time_bcd;
                nsc_time_bcd_q <= nsc_write_time_bcd;
                nsc_write_seq_q <= nsc_write_seq_q + 32'd1;
            end

            case (as_common.araddr)
                CARD_CTRL_REG_SLOT_ENABLE_MASK:   as_client_rdata_q <= card_slot_enable_mask_q;
                CARD_CTRL_REG_FEATURE_ENABLE_MASK: as_client_rdata_q <= card_feature_enable_mask_q;
                CARD_CTRL_REG_SOFTSW_STATE:        as_client_rdata_q <= {11'h000, current_softswitch_state};
                CARD_CTRL_REG_NSC_TIME_LO:         as_client_rdata_q <= nsc_time_bcd_q[31:0];
                CARD_CTRL_REG_NSC_TIME_HI:         as_client_rdata_q <= nsc_time_bcd_q[63:32];
                CARD_CTRL_REG_NSC_WRITE_SEQ:       as_client_rdata_q <= nsc_write_seq_q;
                CARD_CTRL_REG_MENU_CHIME:          as_client_rdata_q <= 32'h0000_0000;
                CARD_CTRL_REG_PHASOR_PAN_LO:       as_client_rdata_q <= {8'h00, phasor_pan_q[23:0]};
                /* CPU1 already polls this word for reset sequencing. Bit 9
                 * marks vTW-owned cycles that need the renderer's selective
                 * one-cycle video-switch lookahead; no extra AXI read is
                 * added to the cycle-render loop. */
                CARD_CTRL_REG_APPLE_RESET_STATUS:  as_client_rdata_q <= {
                    21'h000000, shr_capture_active_w,
                    vtw_video_phase_1mhz,
                    ab_read.res, apple_reset_seq_q
                };
                CARD_CTRL_REG_PHASOR_PAN_HI:       as_client_rdata_q <= {8'h00, phasor_pan_q[47:24]};
                CARD_CTRL_REG_PHASOR_AUDIO:        as_client_rdata_q <= phasor_audio_q;
                CARD_CTRL_REG_ETH_ADDR:            as_client_rdata_q <= {16'h0000, eth_host_addr_q};
                CARD_CTRL_REG_ETH_DATA:            as_client_rdata_q <= {24'h000000, eth_host_wdata_q};
                CARD_CTRL_REG_ETH_CMD:             as_client_rdata_q <= {30'h00000000,
                                                                         eth_host_write_q,
                                                                         1'b0};
                CARD_CTRL_REG_ETH_STATUS:          as_client_rdata_q <= {
                    16'h0000,
                    eth_host_rdata_q,
                    4'h0,
                    eth_host_error_q,
                    eth_host_done_q,
                    eth_host_busy_q,
                    eth_host_ready
                };
                CARD_CTRL_REG_SSC_STATUS:          as_client_rdata_q <= {
                    14'h0000, card_ssc_enable, ssc_tx_overflow,
                    4'h0, ssc_tx_count
                };
                CARD_CTRL_REG_SSC_HEAD:            as_client_rdata_q <= {
                    23'h000000, ssc_tx_head_valid, ssc_tx_head
                };
                CARD_CTRL_REG_SSC_ACIA:            as_client_rdata_q <= {
                    16'h0000, ssc_acia_control, ssc_acia_command
                };
                CARD_CTRL_REG_DISK2_SOUND_BASE:    as_client_rdata_q <= disk2_sound_sample_base_q;
                CARD_CTRL_REG_DISK2_SOUND_CONTROL: as_client_rdata_q <= disk2_sound_control_q;
                8'h13:   as_client_rdata_q <= dbg_apple_access_count;
                8'h14:   as_client_rdata_q <= dbg_apple_miss_count;
                8'h15:   as_client_rdata_q <= dbg_dma_admit_count;
                8'h16:   as_client_rdata_q <= dbg_dma_complete_count;
                8'h18:   as_client_rdata_q <= dbg_serve_late_count;
                8'h1D:   as_client_rdata_q <= dbg_serve_max_latency;
                8'h17:   as_client_rdata_q <= dbg_aux_write_count;
                8'h20:   as_client_rdata_q <= {31'b0, egress_cfg_enable_q};
                8'h21:   as_client_rdata_q <= egress_cfg_ring_base_q;
                8'h22:   as_client_rdata_q <= {27'b0, egress_cfg_ring_size_log2_q};
                8'h23:   as_client_rdata_q <= egress_cfg_producer_ptr_addr_q;
                8'h24:   as_client_rdata_q <= egress_cfg_consumer_ptr_q;
                8'h26:   as_client_rdata_q <= egress_stat_producer_ptr;
                8'h27:   as_client_rdata_q <= egress_stat_records_written;
                8'h28:   as_client_rdata_q <= egress_stat_gap_markers;
                8'h29:   as_client_rdata_q <= egress_stat_bursts_issued;
                8'h2A:   as_client_rdata_q <= egress_stat_full_stall_cycles;
                8'h2B:   as_client_rdata_q <= irqdbg_mouse_word;
                8'h2C:   as_client_rdata_q <= irqdbg_phasor_word;
                /* Bus-capture forensics (write 0x2D clears all of these). */
                8'h2D:   as_client_rdata_q <= busdbg_quality;
                8'h2E:   as_client_rdata_q <= busdbg_tap_mismatch;
                8'h2F:   as_client_rdata_q <= busdbg_strobe_anom;
                8'h30:   as_client_rdata_q <= busdbg_tap_last;
                8'h31:   as_client_rdata_q <= busdbg_mring_q[31:0];
                8'h32:   as_client_rdata_q <= busdbg_mring_q[63:32];
                8'h33:   as_client_rdata_q <= busdbg_mring_q[95:64];
                8'h34:   as_client_rdata_q <= busdbg_mring_q[127:96];
                8'h35:   as_client_rdata_q <= {31'b0, post_main_wide_q};
                8'h3A:   as_client_rdata_q <= busdbg_ghost_write;
                CARD_CTRL_REG_VTW_WR_CHECK: as_client_rdata_q <=
                    vtw_dbg_sync_write_check;
                CARD_CTRL_REG_VTW_WR_ADDR:  as_client_rdata_q <=
                    {16'b0, vtw_dbg_sync_write_addr};
                CARD_CTRL_REG_VTW_C000_CTX: as_client_rdata_q <=
                    vtw_dbg_c000_context;
                CARD_CTRL_REG_VTW_C000_CNT: as_client_rdata_q <=
                    vtw_dbg_c000_counts;
                8'h50:   as_client_rdata_q <= {31'b0, sdd_cfg_enable_q};
                8'h51:   as_client_rdata_q <= sdd_cfg_ring_base_q;
                8'h52:   as_client_rdata_q <= {27'b0, sdd_cfg_ring_size_log2_q};
                8'h53:   as_client_rdata_q <= sdd_cfg_producer_ptr_addr_q;
                8'h54:   as_client_rdata_q <= sdd_cfg_consumer_ptr_q;
                8'h56:   as_client_rdata_q <= sdd_stat_producer_ptr;
                8'h57:   as_client_rdata_q <= sdd_stat_records_written;
                8'h58:   as_client_rdata_q <= sdd_stat_gap_markers;
                8'h59:   as_client_rdata_q <= sdd_stat_bursts_issued;
                8'h5A:   as_client_rdata_q <= sdd_stat_full_stall_cycles;
                CARD_CTRL_REG_ONEE: as_client_rdata_q <= {
                    8'hE1, 8'h00, 1'b1, 1'b0, ONEE_POWER_SENSE_PRESENT,
                    onee_inhibit_reason,
                    onee_physical_isolation_hold,
                    onee_selected,
                    onee_reselect_armed,
                    onee_activity_quiet,
                    onee_activity_lockout,
                    onee_activity_now,
                    onee_force_outputs_off,
                    physical_bus_isolate,
                    onee_enable_effective,
                    onee_request_q
                };
                8'h60:   as_client_rdata_q <= {29'b0,
                                               machine_m2sel_active_high,
                                               machine_mode_q};
                8'h61:   as_client_rdata_q <= {31'b0, aux_provide_en_q};
                8'h62:   as_client_rdata_q <= {31'b0, ramworks_en_q};
                8'h63:   as_client_rdata_q <= {26'b0,
                                               psram_dcount_edge_q,
                                               psram_dcount_q};
                8'h64:   as_client_rdata_q <= {25'b0,
                                               res_external_seen_sticky,
                                               res_internal_seen_sticky,
                                               res_seen_sticky,
                                               rstn_dip_sticky};
                8'h66:   as_client_rdata_q <= dbg_lost_cycle_count;
                8'h67:   as_client_rdata_q <= dbg_wq_drop_count;
                8'h68:   as_client_rdata_q <= {16'b0, dbg_wq_state};
                8'h69:   as_client_rdata_q <= {8'b0, dbg_miss_ctx};
                8'h6A:   as_client_rdata_q <= {30'b0, bm_aux_status};
                8'h65:   as_client_rdata_q <= {26'b0,
                                               sss.c8_select,
                                               sss.c8_internal_rom,
                                               sss.sw_slotc3rom,
                                               sss.sw_intcxrom};
                CARD_CTRL_REG_SS_REGS_LO:   as_client_rdata_q <= ss_regs[31:0];
                CARD_CTRL_REG_SS_REGS_HI:   as_client_rdata_q <= ss_regs[63:32];
                CARD_CTRL_REG_SS_STATUS:    as_client_rdata_q <=
                    {6'b0, ss_vdp_overlay, ss_apple_video, ss_frame, ss_status};
                CARD_CTRL_REG_SS_VRAM_DATA: as_client_rdata_q <= {24'b0, ss_vram_data};
                CARD_CTRL_REG_VTW_CTRL:        as_client_rdata_q <= vtw_ctrl_q;
                CARD_CTRL_REG_VTW_SLOWDOWN:    as_client_rdata_q <= vtw_slowdown_q;
                CARD_CTRL_REG_VTW_SHADOW_ADDR: as_client_rdata_q <= {14'b0, vtw_sh_addr_q};
                CARD_CTRL_REG_VTW_SHADOW_DATA: as_client_rdata_q <= {24'b0, vtw_sh_rdata_q};
                CARD_CTRL_REG_VTW_SYNC_CMD:    as_client_rdata_q <= {7'b0,
                                                                    vtw_arm_rw_q,
                                                                    vtw_arm_wdata_q,
                                                                    vtw_arm_addr_q};
                CARD_CTRL_REG_VTW_SYNC_STATUS: as_client_rdata_q <= {22'b0,
                                                                    vtw_sync_done_q,
                                                                    vtw_arm_busy,
                                                                    vtw_arm_rdata};
                CARD_CTRL_REG_VTW_STATUS:      as_client_rdata_q <= {vtw_dbg_core_pc,
                                                                    vtw_dbg_vsss,
                                                                    vtw_ctrl_q[1],
                                                                    vtw_enable_eff,
                                                                    vtw_bus_owned,
                                                                    vtw_c074_state};
                CARD_CTRL_REG_VTW_CNT_CORE:    as_client_rdata_q <= vtw_cnt_core_cycles;
                CARD_CTRL_REG_VTW_CNT_BUS:     as_client_rdata_q <= vtw_cnt_bus_cycles;
                CARD_CTRL_REG_VTW_CNT_POSTED:  as_client_rdata_q <= vtw_cnt_posted_writes;
                CARD_CTRL_REG_VTW_POST_STATS:  as_client_rdata_q <= {vtw_cnt_post_drops[5:0],
                                                                    vtw_post_high_water,
                                                                    6'b0,
                                                                    vtw_post_fill};
                CARD_CTRL_REG_VTW_CNT_INVALID: as_client_rdata_q <= vtw_cnt_invalid_routes;
                CARD_CTRL_REG_VTW_LAST_SYNC:   as_client_rdata_q <= {vtw_dbg_irq_edges,
                                                                    vtw_dbg_last_sync_rw,
                                                                    vtw_dbg_last_sync_data,
                                                                    vtw_dbg_last_sync_addr};
                CARD_CTRL_REG_VTW_POST_STATUS: as_client_rdata_q <= {vtw_arm_post_ready,
                                                                    vtw_arm_post_accept_count_q};
                CARD_CTRL_REG_VTW_RW_FLUSH:    as_client_rdata_q <= {vtw_arm_rw_flush_busy_q,
                                                                    vtw_arm_rw_hold_state,
                                                                    vtw_arm_rw_flush_count_q};
                CARD_CTRL_REG_VTW_SHADOW_DATA4_STATUS:
                    as_client_rdata_q <= {vtw_sh_word_ready,
                                          vtw_sh_word_busy,
                                          vtw_sh_word_accept_count};
                CARD_CTRL_REG_VTW_SHADOW_READ4_DATA:
                    as_client_rdata_q <= vtw_sh_word_read_data;
                CARD_CTRL_REG_VTW_SHADOW_READ4_STATUS:
                    as_client_rdata_q <= {vtw_sh_word_read_ready,
                                          vtw_sh_word_read_busy,
                                          vtw_sh_word_read_count};
                CARD_CTRL_REG_VTW_CXXX_RING0:  as_client_rdata_q <= vtw_dbg_cxxx_ring[31:0];
                CARD_CTRL_REG_VTW_CXXX_RING1:  as_client_rdata_q <= vtw_dbg_cxxx_ring[63:32];
                CARD_CTRL_REG_VTW_CXXX_RING2:  as_client_rdata_q <= vtw_dbg_cxxx_ring[95:64];
                CARD_CTRL_REG_VTW_CXXX_RING3:  as_client_rdata_q <= vtw_dbg_cxxx_ring[127:96];
                CARD_CTRL_REG_VTW_C0_RING0:    as_client_rdata_q <= vtw_dbg_c0_ring[31:0];
                CARD_CTRL_REG_VTW_C0_RING1:    as_client_rdata_q <= vtw_dbg_c0_ring[63:32];
                CARD_CTRL_REG_VTW_C0_RING2:    as_client_rdata_q <= vtw_dbg_c0_ring[95:64];
                CARD_CTRL_REG_VTW_C0_RING3:    as_client_rdata_q <= vtw_dbg_c0_ring[127:96];
                CARD_CTRL_REG_VTW_TRACE_STATUS: as_client_rdata_q <= vtw_dbg_trace_status;
                CARD_CTRL_REG_VTW_BUS_FAULTS:   as_client_rdata_q <= vtw_dbg_bus_faults;
                8'h82: as_client_rdata_q <= vtw_dbg_io_trace[31:0];
                8'h83: as_client_rdata_q <= vtw_dbg_io_trace[63:32];
                8'h84: as_client_rdata_q <= vtw_dbg_io_trace[95:64];
                8'h85: as_client_rdata_q <= vtw_dbg_io_trace[127:96];
                8'h86: as_client_rdata_q <= vtw_dbg_io_trace[159:128];
                8'h87: as_client_rdata_q <= vtw_dbg_io_trace[191:160];
                8'h88: as_client_rdata_q <= vtw_dbg_io_trace[223:192];
                8'h89: as_client_rdata_q <= vtw_dbg_io_trace[255:224];
                8'h8A: as_client_rdata_q <= vtw_dbg_io_trace[287:256];
                8'h8B: as_client_rdata_q <= vtw_dbg_io_trace[319:288];
                8'h8C: as_client_rdata_q <= vtw_dbg_io_trace[351:320];
                8'h8D: as_client_rdata_q <= vtw_dbg_io_trace[383:352];
                8'h8E: as_client_rdata_q <= vtw_dbg_io_trace[415:384];
                8'h8F: as_client_rdata_q <= vtw_dbg_io_trace[447:416];
                8'h90: as_client_rdata_q <= vtw_dbg_io_trace[479:448];
                8'h91: as_client_rdata_q <= vtw_dbg_io_trace[511:480];
                8'h92: as_client_rdata_q <= vtw_dbg_pc_trace[31:0];
                8'h93: as_client_rdata_q <= vtw_dbg_pc_trace[63:32];
                8'h94: as_client_rdata_q <= vtw_dbg_pc_trace[95:64];
                8'h95: as_client_rdata_q <= vtw_dbg_pc_trace[127:96];
                8'h96: as_client_rdata_q <= vtw_dbg_pc_trace[159:128];
                8'h97: as_client_rdata_q <= vtw_dbg_pc_trace[191:160];
                8'h98: as_client_rdata_q <= vtw_dbg_pc_trace[223:192];
                8'h99: as_client_rdata_q <= vtw_dbg_pc_trace[255:224];
                default: as_client_rdata_q <= 32'h00000000;
            endcase
        end
    end
    assign as_client.rdata = as_client_rdata_q;

endmodule
