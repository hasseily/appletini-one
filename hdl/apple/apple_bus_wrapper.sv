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

    /* Machine-mode interlock: INH and DMA may only be driven after the PS
     * positively identifies a
     * machine where they are safe (//e or II/II+). Low = GS/UNKNOWN =
     * never drive them; the GS INH anomaly makes a stray INH assert a
     * bus-contention event, and DMA takeover is //e-only by policy. */
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
    localparam int TAP_DATA_SNAP        = 59; // sample data from bus

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
    logic [8:0]  misc_clean;
    assign {rw_clean, phi0_clean, m2sel_clean, m2b0_clean,
            inh_clean, res_clean, irq_clean, rdy_clean, dma_clean} = misc_clean;

    /* RES# hysteresis filter. The raw synced sample is re-snapped every fclk,
     * so a sub-cycle dip could reset the soft-switch tracker even though a
     * 6502 requires RES# across clock phases. Require about 460 ns of
     * continuous low; real resets last milliseconds and pass untouched. */
    localparam int RES_FILTER_LOW_TAPS = 64;
    logic [6:0] res_low_cnt;
    logic       res_filtered;
    assign res_filtered_out = res_filtered;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            res_low_cnt  <= '0;
            res_filtered <= 1'b1;
        end else if (res_clean) begin
            res_low_cnt  <= '0;
            res_filtered <= 1'b1;
        end else if (res_low_cnt == 7'(RES_FILTER_LOW_TAPS)) begin
            res_filtered <= 1'b0;
        end else begin
            res_low_cnt <= res_low_cnt + 7'd1;
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
    cdc_bus_iob #(.WIDTH(9)) misc_clean_sync (
        .clk(clk), .resetn(rstn),
        .din({apple_rw_pin, apple_phi0_pin, apple_m2sel_pin,
              apple_m2b0_pin, apple_inh_pin, apple_res_pin,
              apple_irq_pin, apple_rdy_pin, apple_dma_pin}),
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
    wire data_phase_snap_bus     = data_pipe[TAP_DATA_SNAP];

    // ------------------------------------------------------------------
    // Pin drivers
    // ------------------------------------------------------------------
    wire apple_data_enable    = bus_emit_state && ab_write.wr_data_en;
    // Address and R/W drive only while an arbiter client explicitly requests
    // ownership; otherwise both buses remain tri-stated.
    wire apple_addr_rw_enable = ab_write.wr_addr_rw_en;

    assign apple_data_pin = apple_data_enable    ? ab_write.wr_data : 8'hzz;
    assign apple_addr_pin = apple_addr_rw_enable ? ab_write.wr_addr : 16'hzzzz;
    assign apple_rw_pin   = apple_addr_rw_enable ? ab_write.wr_rw   : 1'hz;

    // IRQ is open-collector and timing-insensitive
    assign apple_irq_pin = ab_write.assert_irq ? 1'b0 : 1'bz;
    // INH must only flip in the addr-phase window, so it's registered
    assign apple_inh_pin = (apple_inh_assert && inh_allowed)
                                                ? 1'b0 : 1'bz;
    // Appletini does not drive these motherboard control lines.
    assign apple_res_pin = 1'bz;
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

            // Sample the bus at the right phase taps
            if (addr_phase_snap_bus) begin
                ab_read_r.addr        <= addr_clean;
                ab_read_r.rw          <= rw_clean;
                ab_read_r.m2sel       <= m2sel_clean;
                ab_read_r.m2b0        <= m2b0_clean;
                ab_read_r.inh         <= inh_clean;
                ab_read_r.irq         <= irq_clean;
                ab_read_r.rdy         <= rdy_clean;
                ab_read_r.dma         <= dma_clean;
                ab_read_r.cycle_valid <= cycle_valid_now;
                ab_read_r.addr_en     <= 1'b1;
                addr_en_emitted_q     <= 1'b1;
            end
            else if (addr_phase_sss_ready) begin
                /* Decode strobe: suppressed whole for M2-invalid cycles
                 * so every downstream decoder inherits qualification. */
                ab_read_r.sss_en  <= ab_read_r.cycle_valid;
            end
            else if (data_phase_snap_bus) begin
                ab_read_r.data    <= data_clean;
                ab_read_r.data_en <= ab_read_r.cycle_valid;
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

endmodule
