`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module: apple_bus_wrapper
//
// Bridges the Apple II edge-connector signals to the FPGA-internal AppleBus_read
// / AppleBus_write structs. Synchronizes the asynchronous bus pins into the FPGA
// clock domain, generates phase-delayed timing markers via shift-register pipes,
// and gates the FPGA's data-out / inh-assert / addr-out drivers to respect the
// 6502 bus timing windows.
//////////////////////////////////////////////////////////////////////////////////

module apple_bus_wrapper (
    input  logic                  clk,
    input  logic                  rstn,
    /* Filtered Apple RES# (post-hysteresis), exported for the reset
     * forensics stickies in apple_top. */
    output logic                  res_filtered_out,
    output logic [31:0]           dbg_lost_cycle_count,

    /* Bus-capture forensics (II+ ghost-write investigation). Saturating
     * counters + a last-offender snapshot; dbg_clear zeroes them all.
     *   dbg_bus_quality  {ring events[15:0], short edges[15:0]}
     *   dbg_tap_mismatch {read mismatches[15:0], write mismatches[15:0]}
     *   dbg_strobe_anom  {extra data_en[15:0], missing data_en[15:0]}
     *   dbg_tap_last     {addr[15:0], early byte[7:0], late byte[7:0]} */
    output logic [31:0]           dbg_bus_quality,
    output logic [31:0]           dbg_tap_mismatch,
    output logic [31:0]           dbg_strobe_anom,
    output logic [31:0]           dbg_tap_last,
    input  logic                  dbg_clear,

    /* Machine-mode interlock: INH and DMA may only be driven after the PS
     * positively identifies a supported machine (//e or II/II+). Low =
     * GS/UNKNOWN = never drive them; the GS INH anomaly makes a stray INH
     * assert a bus-contention event. vTW bus takeover is supported on both
     * identified //e and II/II+ hosts. */
    input  logic                  inh_allowed,

    /* M2SEL qualification. When gs_m2_qualify is high for an identified
     * IIgs, a bus cycle is
     * only decodable if M2SEL was asserted at the address snap; the
     * sss_en/data_en strobes are suppressed for all other cycles and
     * ab_read.cycle_valid reports the verdict for addr_en-keyed
     * early decoders. m2sel_active_high selects the sampled polarity;
     * IIgs /M2SEL is active low unless board logic inverts it. */
    input  logic                  gs_m2_qualify,
    input  logic                  m2sel_active_high,
    input  logic                  host_is_iiplus,
    input  logic [5:0]            iiplus_data_tap,

    // Apple II edge connector
    inout  wire  [7:0]            apple_data_pin,
    inout  wire  [15:0]           apple_addr_pin,
    inout  wire                   apple_rw_pin,
    input  logic                  apple_phi0_pin,
    input  logic                  apple_m2sel_pin,
    input  logic                  apple_m2b0_pin,
    inout  wire                   apple_inh_pin,
    inout  wire                   apple_res_pin,
    inout  wire                   apple_irq_pin,
    inout  wire                   apple_rdy_pin,
    inout  wire                   apple_dma_pin,
    inout  wire                   apple_nmi_pin,

    // Tini board control
    output logic                  tini_oe_pin,
    input  logic                  tini_5v_pin,
    output logic                  tini_addr_dir_pin,
    output logic                  tini_data_dir_pin,

    // Internal bus
    output globals::AppleBus_read ab_read,
    input  globals::AppleBus_write ab_write
);

    // ------------------------------------------------------------------
    // Phase timing constants (shift-register tap positions)
    // ------------------------------------------------------------------
    localparam int ADDR_PIPE_DEPTH      = 40;
    localparam int DATA_PIPE_DEPTH      = 62;
    localparam int TAP_ADDR_BEGIN       = 3;
    localparam int TAP_ADDR_SNAP        = 25; // sample addr/misc after phi falling
    localparam int TAP_SSS_READY        = 26; // soft-switch decode latched
    localparam int TAP_INH_DEADLINE     = 32; // last chance to assert INH
    localparam int TAP_DATA_EMIT        = 25; // FPGA may drive data
    /* THE address sample point, just inside PHI0-high (~60 ns after the
     * detected rise, ~90 ns after the true rise once the majority
     * filter's lag is counted). The window is bounded on BOTH sides by
     * real masters: a DMA posted-write engine (TransWarp write-through)
     * asserts addr/RW only around the PHI0 rise but must have them valid
     * by the IIe row-address latch (~rise+40 ns); the TransWarp's
     * CPU-synchronized cycles assert during PHI1 like a 6502 but RELEASE
     * the bus around the rise, after which the level-shifted bus decays.
     * A2DVI samples the same bus at +42/+114 ns and is field-proven on
     * TransWarp hardware -- this tap sits inside that proven window.
     * (Sampling later, ~+180 ns, lands on the decayed bus and corrupts
     * every CPU-synchronized TransWarp cycle.) TAP_ADDR_SNAP
     * remains as the early snapshot feeding the INH/PSRAM serving path,
     * whose assert deadline falls mid-PHI1. */
    localparam int TAP_ADDR_SNAP_LATE   = 8;
    localparam int TAP_DATA_SNAP        = 59; // sample data from bus (IIe/accel)
    /* II/II+ hosts drive the data bus on a different schedule than the //e
     * (discrete timing, real 6502, no IOU row-address latch), so their data
     * settles earlier in the window. On a II/II+ the sample point is taken
     * from the runtime `iiplus_data_tap` input (PS-tunable, default 52) so
     * the exact value can be swept on hardware without a rebuild. */
    /* Bus-master address-drive point, early PHI1 (~60 ns after the detected
     * fall). A master updating its driven addr/R-W here presents ideal 6502
     * timing: asserted long before the IIe row-address latch, held through
     * the whole cycle. Exported as ab_read.drive_en for vtw_bus_engine. */
    localparam int TAP_DRIVE_ADDR       = 8;

    // ------------------------------------------------------------------
    // CDC-synchronized pin samples
    // ------------------------------------------------------------------
    logic [7:0]  data_clean;
    logic [15:0] addr_clean;

    // misc_clean is unpacked into named signals immediately to avoid
    // bit-position drift between the cdc input concatenation and consumers.
    logic        rw_clean;
    logic        phi0_clean;
    logic        m2sel_clean;
    logic        m2b0_clean;
    logic        inh_clean;
    logic        res_clean;
    logic        irq_clean;
    logic        rdy_clean;
    logic        dma_clean;
    logic        nmi_clean;
    logic [9:0]  misc_clean;
    assign {rw_clean, phi0_clean, m2sel_clean, m2b0_clean,
            inh_clean, res_clean, irq_clean, rdy_clean, dma_clean,
            nmi_clean} = misc_clean;

    /* RES# hysteresis filter, symmetric: ~2 us of continuous level in
     * either direction before the filtered value changes. The //e reset
     * line has slow RC edges; the motherboard's own chips (6502, MMU,
     * IOU) qualify RES# over microseconds, and every consumer here --
     * cards, soft-switch trackers, the vTW core -- must count exactly
     * the same resets those chips count. An observer more sensitive than
     * the motherboard would honor edge chatter the machine ignores and
     * its reset-cleared state would diverge from the MMU's. */
    localparam int RES_FILTER_TAPS = 280;   // ~2.1 us at 133.333 MHz
    logic [8:0] res_lvl_cnt;
    logic       res_filtered;
    assign res_filtered_out = res_filtered;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            res_lvl_cnt  <= '0;
            res_filtered <= 1'b1;
        end else if (res_clean == res_filtered) begin
            res_lvl_cnt <= '0;
        end else if (res_lvl_cnt == 9'(RES_FILTER_TAPS)) begin
            res_filtered <= res_clean;
            res_lvl_cnt  <= '0;
        end else begin
            res_lvl_cnt <= res_lvl_cnt + 9'd1;
        end
    end

    // IOB-captured samplers: stage 0 lives in the pad's IOB register so
    // bus sample timing is identical in every build (see cdc_bus_iob).
    // All three MUST use the same sampler depth so addr/data/misc (incl.
    // PHI0) stay mutually aligned and the TAP_* constants hold.
    cdc_bus_iob #(.WIDTH(8)) data_clean_sync (
        .clk(clk), .resetn(rstn),
        .din(apple_data_pin), .dout(data_clean)
    );
    cdc_bus_iob #(.WIDTH(16)) addr_clean_sync (
        .clk(clk), .resetn(rstn),
        .din(apple_addr_pin), .dout(addr_clean)
    );
    cdc_bus_iob #(.WIDTH(10)) misc_clean_sync (
        .clk(clk), .resetn(rstn),
        .din({apple_rw_pin, apple_phi0_pin, apple_m2sel_pin,
              apple_m2b0_pin, apple_inh_pin, apple_res_pin,
              apple_irq_pin, apple_rdy_pin, apple_dma_pin,
              apple_nmi_pin}),
        .dout(misc_clean)
    );

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    /* A five-sample PHI0 majority filter rejects sub-three-sample ringing
     * without requiring a fully stable history window. It can shift an edge
     * by a few fabric clocks but cannot suppress a complete Apple cycle. */
    logic [4:0] phi0_maj_hist;
    logic       phi0_filt;
    logic       prev_phi0_filt;
    /* Cycle-integrity forensics: every phi0 fall must be followed by
     * exactly one addr_en strobe before the next fall. A fall arriving
     * with the previous cycle's strobe never emitted = one silently
     * lost bus cycle. (In GS/M2-qualified mode suppressed cycles would
     * count here by design -- IIe mode expects zero, always.) */
    logic        addr_en_emitted_q;
    logic [31:0] lost_cycle_count_q;
    assign dbg_lost_cycle_count = lost_cycle_count_q;
    wire [2:0]  phi0_ones = {2'b0, phi0_maj_hist[0]} +
                            {2'b0, phi0_maj_hist[1]} +
                            {2'b0, phi0_maj_hist[2]} +
                            {2'b0, phi0_maj_hist[3]} +
                            {2'b0, phi0_maj_hist[4]};
    wire m2sel_asserted = m2sel_active_high ? m2sel_clean : ~m2sel_clean;
    wire cycle_valid_now = !gs_m2_qualify || m2sel_asserted;
    logic [ADDR_PIPE_DEPTH-1:0]    addr_pipe;
    logic [DATA_PIPE_DEPTH-1:0]    data_pipe;
    globals::AppleBus_read         ab_read_r;
    logic                          bus_emit_state;
    logic                          apple_inh_assert;
    /* IRQ/NMI glitch filters: previous data-snap samples (active low).
     * See the sampling note at the data-snap branch below. */
    logic                          irq_snap_q;
    logic                          nmi_snap_q;

    // ------------------------------------------------------------------
    // Edge / phase markers (combinational view of current cycle)
    // ------------------------------------------------------------------
    wire phi0_rise               = phi0_filt && !prev_phi0_filt;
    wire phi0_fall               = !phi0_filt && prev_phi0_filt;
    wire addr_phase_begin        = addr_pipe[TAP_ADDR_BEGIN];
    wire addr_phase_snap_bus     = addr_pipe[TAP_ADDR_SNAP];
    wire addr_phase_sss_ready    = addr_pipe[TAP_SSS_READY];
    // INH deadline is timed late enough (~12 cycles past addr_ready) to
    // give downstream decode time to settle before the bus latches it.
    wire addr_phase_inh_deadline = addr_pipe[TAP_INH_DEADLINE];
    wire data_phase_emit         = data_pipe[TAP_DATA_EMIT];
    wire addr_phase_snap_late    = data_pipe[TAP_ADDR_SNAP_LATE];
    wire data_phase_snap_bus     = host_is_iiplus ? data_pipe[iiplus_data_tap]
                                                   : data_pipe[TAP_DATA_SNAP];
    wire addr_phase_drive        = addr_pipe[TAP_DRIVE_ADDR];

    // ------------------------------------------------------------------
    // Pin drivers
    // ------------------------------------------------------------------
    /* Outbound data drive. On a //e, card clients still drop wr_data_en at
     * data_en (tap 59), while the pin driver below enforces the physical
     * PHI0 boundary. Do not extend that drive to filtered phi0_fall here:
     * phi0_fall is produced after IOB capture, synchronization, majority
     * filtering, and edge detection, so it occurs well after the physical
     * falling edge and would make the external transceiver contend with the
     * motherboard in PHI1.
     *
     * The earlier II/II+ data tap is still under investigation. Preserve its
     * existing byte hold for now, but isolate it completely from the //e
     * path so II/II+ experiments cannot perturb known-good //e timing. */
    wire drive_live = bus_emit_state && ab_write.wr_data_en;
    logic       drive_hold_q;
    logic [7:0] drive_data_q;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            drive_hold_q <= 1'b0;
            drive_data_q <= 8'h00;
        end else if (phi0_fall) begin
            drive_hold_q <= 1'b0;
        end else if (drive_live) begin
            drive_hold_q <= 1'b1;
            drive_data_q <= ab_write.wr_data;
        end
    end
    // Address and R/W drive only while an arbiter client explicitly requests
    // ownership; otherwise both buses remain tri-stated.
    wire apple_addr_rw_enable = ab_write.wr_addr_rw_en;

    /* On the //e, the physical PHI0 level is the hard electrical boundary
     * for native card-serving cycles. The synchronized/filtered PHI0 markers
     * are intentionally suitable for internal sequencing, not for releasing
     * an external bus: their latency extends into PHI1. vTW-owned cycles drive
     * address/RW as well and retain the bus engine's explicit drive window.
     * The direction-output route is bounded in the XDC so the native-cycle
     * asynchronous release remains placement-independent. */
    wire drive_live_at_pin    = drive_live &&
                                (host_is_iiplus || apple_addr_rw_enable ||
                                 apple_phi0_pin);
    wire drive_hold_active    = host_is_iiplus && drive_hold_q;
    wire apple_data_enable    = drive_live_at_pin || drive_hold_active;

    assign apple_data_pin = drive_live_at_pin ? ab_write.wr_data :
                            drive_hold_active ? drive_data_q     : 8'hzz;
    assign apple_addr_pin = apple_addr_rw_enable ? ab_write.wr_addr : 16'hzzzz;
    assign apple_rw_pin   = apple_addr_rw_enable ? ab_write.wr_rw   : 1'hz;

    /* IRQ is open-collector and timing-insensitive. The production board has
     * no dedicated IRQ assertion transistor: U19 (the planned A2CTRL.IRQ pin)
     * is physically unconnected. Assert through the bidirectional IRQ lane by
     * driving only low or high-Z; the external pull-up provides release. */
    assign apple_irq_pin = ab_write.assert_irq ? 1'b0 : 1'bz;
    // INH must only flip in the addr-phase window, so it's registered
    assign apple_inh_pin = (apple_inh_assert && inh_allowed)
                                                ? 1'b0 : 1'bz;
    /* RES# is open-collector like a keyboard CTRL-RESET: pulled low on
     * request, released to the motherboard pull-up otherwise. The only
     * client asserting it is the vTW session start, which resets the
     * whole machine (cards, MMU, PS reset detection) before taking over
     * so a takeover can never land mid-protocol. */
    assign apple_res_pin = ab_write.assert_res ? 1'b0 : 1'bz;
    // Appletini does not drive these motherboard control lines.
    assign apple_rdy_pin = 1'bz;
    assign apple_nmi_pin = 1'bz;
    // DMA# is open-drain: any client asserting takes the line low; nobody
    // asserting leaves it tri-state (pulled high by the motherboard).
    assign apple_dma_pin = (ab_write.assert_dma && inh_allowed)
                                                ? 1'b0 : 1'bz;

    // Tini board glue
    assign tini_oe_pin       = tini_5v_pin;
    assign tini_data_dir_pin = apple_data_enable;
    // Tini board's addr direction follows our addr-drive enable.
    assign tini_addr_dir_pin = apple_addr_rw_enable;
    assign ab_read           = ab_read_r;

    // ------------------------------------------------------------------
    // Sequential
    // ------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (!rstn) begin
            addr_en_emitted_q  <= 1'b1;
            lost_cycle_count_q <= 32'd0;
            phi0_maj_hist    <= '0;
            phi0_filt        <= 1'b0;
            prev_phi0_filt   <= 1'b0;
            addr_pipe        <= '0;
            data_pipe        <= '0;
            ab_read_r        <= '0;
            /* Interrupt lines idle deasserted (active low). */
            ab_read_r.irq    <= 1'b1;
            ab_read_r.nmi    <= 1'b1;
            irq_snap_q       <= 1'b1;
            nmi_snap_q       <= 1'b1;
            bus_emit_state   <= 1'b0;
            apple_inh_assert <= 1'b0;
        end
        else begin
            // Track phi0 and inject edge markers into the timing pipes
            phi0_maj_hist  <= {phi0_maj_hist[3:0], phi0_clean};
            phi0_filt      <= (phi0_ones >= 3'd3);
            prev_phi0_filt <= phi0_filt;
            addr_pipe <= {addr_pipe[ADDR_PIPE_DEPTH-2:0], phi0_fall};
            if (phi0_fall) begin
                if (!addr_en_emitted_q) begin
                    lost_cycle_count_q <= lost_cycle_count_q + 32'd1;
                end
                addr_en_emitted_q <= 1'b0;
            end
            data_pipe <= {data_pipe[DATA_PIPE_DEPTH-2:0], phi0_rise};

            // Always re-snap timing-insensitive global pins into the read struct.
            // RES# is not an address-phase signal; cards must see it even if the
            // 6502 is not presenting a normal bus cycle during reset.
            ab_read_r.phi0          <= phi0_clean;
            ab_read_r.res           <= res_filtered;
            ab_read_r.addr_en       <= 1'b0;
            ab_read_r.data_en       <= 1'b0;
            ab_read_r.sss_en        <= 1'b0;
            ab_read_r.serve_en      <= 1'b0;
            /* Independent of the sample chain below: fires every cycle at
             * the drive tap regardless of M2 qualification (a bus master
             * must keep the bus parked even through invalid cycles). */
            ab_read_r.drive_en      <= addr_phase_drive;

            // Sample the bus at the right phase taps
            if (addr_phase_snap_bus) begin
                /* Early PHI1 snapshot: feeds ONLY the INH/PSRAM serving
                 * path (its assert deadline falls mid-PHI1). A DMA bus
                 * master may still be showing the previous cycle here. */
                ab_read_r.addr_early  <= addr_clean;
                ab_read_r.rw_early    <= rw_clean;
                ab_read_r.m2sel       <= m2sel_clean;
                ab_read_r.m2b0        <= m2b0_clean;
                ab_read_r.inh         <= inh_clean;
                ab_read_r.rdy         <= rdy_clean;
                ab_read_r.dma         <= dma_clean;
                ab_read_r.cycle_valid <= cycle_valid_now;
                ab_read_r.addr_en     <= 1'b1;
                addr_en_emitted_q     <= 1'b1;
            end
            else if (addr_phase_sss_ready) begin
                /* Early-decode strobe: suppressed whole for M2-invalid
                 * cycles so every downstream decoder inherits
                 * qualification. */
                ab_read_r.sss_en  <= ab_read_r.cycle_valid;
            end
            else if (addr_phase_snap_late) begin
                /* THE authoritative address sample, inside PHI0-high:
                 * any bus master the motherboard DRAM can accept has
                 * addr/RW valid here (6502 or DMA engine alike).
                 * Everything except INH/PSRAM serving keys off this. */
                ab_read_r.addr     <= addr_clean;
                ab_read_r.rw       <= rw_clean;
                ab_read_r.serve_en <= ab_read_r.cycle_valid;
            end
            else if (data_phase_snap_bus) begin
                ab_read_r.data    <= data_clean;
                ab_read_r.data_en <= ab_read_r.cycle_valid;
                /* IRQ/NMI sampling: late PHI0, the same phase a real
                 * 6502 samples IRQ (end of phi2) -- the bus has been
                 * quiet for ~800 ns. The early-PHI1 point used before
                 * landed ~100 ns after this card slams 17 address/R-W
                 * lines through the level shifters; the resulting ground
                 * bounce on the pulled-up open-collector lines produced
                 * phantom interrupt samples. Nothing consumed these
                 * fields until the vTW core, so the hazard was invisible
                 * (RES# grew its hysteresis filter for the same reason).
                 * A line must be low on TWO consecutive snaps to read
                 * asserted: real Apple interrupt sources hold the line
                 * until serviced, so the extra microsecond of latency is
                 * unobservable while single-sample glitches are dead. */
                ab_read_r.irq <= irq_clean | irq_snap_q;
                ab_read_r.nmi <= nmi_clean | nmi_snap_q;
                irq_snap_q    <= irq_clean;
                nmi_snap_q    <= nmi_clean;
            end

            // Drive-window state machine
            if (addr_phase_begin) begin
                bus_emit_state   <= 1'b0;
                apple_inh_assert <= 1'b0;
            end
            if (data_phase_emit) begin
                bus_emit_state   <= 1'b1;
            end
            if (addr_phase_inh_deadline) begin
                apple_inh_assert <= ab_write.assert_inh;
            end
        end
    end

    // ------------------------------------------------------------------
    // Bus-capture forensics (II+ ghost-write investigation). Three
    // independent hazard meters, all off the timing-critical paths:
    //  * ring events: two or more transitions inside the 5-sample PHI0
    //    majority window -- edge noise the filter is actively fighting.
    //    Counted once per burst.
    //  * short edges: filtered PHI0 edges closer than 40 clk (~300 ns;
    //    a real half-period is ~65 clk) -- noise that BEAT the filter
    //    and minted a phantom cycle.
    //  * data_en pairing: PHI0 periods with zero or more than one data
    //    strobe. dbg_lost_cycle_count only sees missing addr strobes;
    //    this catches extras.
    //  * tap mismatch: bus data at tap 46 vs tap 58 disagreeing within
    //    one cycle = the data window is not stable where the host tap
    //    (52 on II/II+, 59 on //e) samples. The last offender's address
    //    and both bytes are latched. Self-served cycles are excluded:
    //    the card releases its drive when data_en fires, so with a host
    //    tap below 58 the late debug sample would see the released bus
    //    and count a phantom mismatch on every served read.
    // ------------------------------------------------------------------
    localparam int TAP_DATA_DBG_EARLY = 46;
    localparam int TAP_DATA_DBG_LATE  = 58;
    wire dbg_snap_early = data_pipe[TAP_DATA_DBG_EARLY];
    wire dbg_snap_late  = data_pipe[TAP_DATA_DBG_LATE];
    wire [2:0] dbg_hist_trans =
        {2'b0, phi0_maj_hist[4] ^ phi0_maj_hist[3]} +
        {2'b0, phi0_maj_hist[3] ^ phi0_maj_hist[2]} +
        {2'b0, phi0_maj_hist[2] ^ phi0_maj_hist[1]} +
        {2'b0, phi0_maj_hist[1] ^ phi0_maj_hist[0]};
    wire dbg_ringing = (dbg_hist_trans >= 3'd2);

    logic        dbg_ring_prev_q;
    logic [15:0] dbg_ring_cnt_q;
    logic [15:0] dbg_short_cnt_q;
    logic [15:0] dbg_mm_rd_q;
    logic [15:0] dbg_mm_wr_q;
    logic [15:0] dbg_extra_cnt_q;
    logic [15:0] dbg_miss_cnt_q;
    logic [6:0]  dbg_edge_gap_q;
    logic [1:0]  dbg_den_seen_q;
    logic [7:0]  dbg_early_data_q;
    logic        dbg_self_drive_q;
    logic [31:0] dbg_tap_last_q;

    assign dbg_bus_quality  = {dbg_ring_cnt_q, dbg_short_cnt_q};
    assign dbg_tap_mismatch = {dbg_mm_rd_q, dbg_mm_wr_q};
    assign dbg_strobe_anom  = {dbg_extra_cnt_q, dbg_miss_cnt_q};
    assign dbg_tap_last     = dbg_tap_last_q;

    always_ff @(posedge clk) begin
        if (!rstn || dbg_clear) begin
            dbg_ring_prev_q  <= 1'b0;
            dbg_ring_cnt_q   <= 16'd0;
            dbg_short_cnt_q  <= 16'd0;
            dbg_mm_rd_q      <= 16'd0;
            dbg_mm_wr_q      <= 16'd0;
            dbg_extra_cnt_q  <= 16'd0;
            dbg_miss_cnt_q   <= 16'd0;
            dbg_edge_gap_q   <= 7'd127;
            dbg_den_seen_q   <= 2'd0;
            dbg_early_data_q <= 8'd0;
            dbg_self_drive_q <= 1'b0;
            dbg_tap_last_q   <= 32'd0;
        end else begin
            // PHI0 majority-window ringing, one count per burst.
            dbg_ring_prev_q <= dbg_ringing;
            if (dbg_ringing && !dbg_ring_prev_q &&
                dbg_ring_cnt_q != 16'hFFFF) begin
                dbg_ring_cnt_q <= dbg_ring_cnt_q + 16'd1;
            end

            // Filtered-edge spacing.
            if (phi0_rise || phi0_fall) begin
                if (dbg_edge_gap_q < 7'd40 &&
                    dbg_short_cnt_q != 16'hFFFF) begin
                    dbg_short_cnt_q <= dbg_short_cnt_q + 16'd1;
                end
                dbg_edge_gap_q <= 7'd0;
            end else if (dbg_edge_gap_q != 7'd127) begin
                dbg_edge_gap_q <= dbg_edge_gap_q + 7'd1;
            end

            // data_en pulses per PHI0 period (fall to fall).
            if (phi0_fall) begin
                if (dbg_den_seen_q == 2'd0 &&
                    dbg_miss_cnt_q != 16'hFFFF) begin
                    dbg_miss_cnt_q <= dbg_miss_cnt_q + 16'd1;
                end else if (dbg_den_seen_q > 2'd1 &&
                             dbg_extra_cnt_q != 16'hFFFF) begin
                    dbg_extra_cnt_q <= dbg_extra_cnt_q + 16'd1;
                end
                dbg_den_seen_q <= {1'b0, ab_read_r.data_en};
            end else if (ab_read_r.data_en && dbg_den_seen_q != 2'd3) begin
                dbg_den_seen_q <= dbg_den_seen_q + 2'd1;
            end

            // Two-tap data-window probe (self-served cycles excluded).
            if (dbg_snap_early) begin
                dbg_early_data_q <= data_clean;
                dbg_self_drive_q <= apple_data_enable;
            end
            if (dbg_snap_late && !dbg_self_drive_q &&
                data_clean != dbg_early_data_q) begin
                dbg_tap_last_q <= {ab_read_r.addr, dbg_early_data_q,
                                   data_clean};
                if (ab_read_r.rw) begin
                    if (dbg_mm_rd_q != 16'hFFFF) begin
                        dbg_mm_rd_q <= dbg_mm_rd_q + 16'd1;
                    end
                end else if (dbg_mm_wr_q != 16'hFFFF) begin
                    dbg_mm_wr_q <= dbg_mm_wr_q + 16'd1;
                end
            end
        end
    end

endmodule
