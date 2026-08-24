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

module apple_bus_wrapper #(
    /* The integrated top can isolate the address-owner bit in its final
     * low-fanout cone. Stand-alone benches and other users keep the wrapper's
     * own direct address kill. */
    parameter bit ADDR_OWNER_PREISOLATED = 1'b0
) (
    input  logic                  clk,
    input  logic                  rstn,
    /* Direct physical-output kill for stand-alone ONE//e mode. This gate
     * must not depend on PHI0: the host clock may already be stopped while a
     * registered INH or data response remains armed. */
    input  logic                  physical_bus_isolate,
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
    /*   dbg_ghost_write  {ghost writes[15:0], last ghost addr[15:0]}:
     *   owned-bus cycles (a master holds /DMA without driving address/R-W)
     *   whose R/W still sampled LOW at the production data tap. Each count
     *   is a cycle the motherboard decoded as a WRITE no master issued --
     *   floating-bus residue matured into a rogue write. Must stay 0. */
    output logic [31:0]           dbg_ghost_write,
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
    /* Active vTW session latched as II/II+. apple_top holds this verdict
     * across Apple RESET, when live machine identification is unavailable.
     * Every such session automatically refreshes the translated /DMA hold. */
    input  logic                  iiplus_dma_refresh_active,

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
    localparam int TAP_DATA_SNAP        = 59; // sample data from bus
    /* II+ IRQ re-arm: release the otherwise level-low IRQ for four fabric
     * clocks (~30 ns), then reassert at the hardware-proven falling-edge
     * point. This gives the translator enough recovery time while keeping its
     * refreshed falling edge close to the CPU IRQ sample. */
    localparam int TAP_IRQ_REARM_RELEASE = TAP_DATA_SNAP - 8;
    localparam int TAP_IRQ_REARM_ASSERT  = TAP_DATA_SNAP - 4;
    /* II+ /DMA re-arm uses the same late-PHI0 notch. /DMA is low again before
     * the next ownership boundary, while the fresh falling edge retriggers
     * the TXS lane's edge accelerator. */
    localparam int TAP_DMA_REARM_RELEASE = TAP_DATA_SNAP - 8;
    localparam int TAP_DMA_REARM_ASSERT  = TAP_DATA_SNAP - 4;
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
    (* KEEP = "TRUE" *) logic       irq_rearm_release_q;
    (* KEEP = "TRUE" *) logic       dma_rearm_release_q;
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
    wire data_phase_snap_bus     = data_pipe[TAP_DATA_SNAP];
    wire addr_phase_drive        = addr_pipe[TAP_DRIVE_ADDR];

    // ------------------------------------------------------------------
    // Pin drivers
    // ------------------------------------------------------------------
    /* Outbound data drive. The physical PHI0 pin remains the hard electrical
     * release boundary for ordinary writes. II/II+ card READ responses alone
     * get a short saved-byte hold through the synchronized fall: that restores
     * the margin required by the unbuffered motherboard without allowing a
     * vTW or posted write to leak stale data into PHI1.
     *
     * A //e vTW-owned cycle retains the established explicit engine window.
     * Its buffered bus is isolated under /DMA and this path is already
     * hardware-validated.
     *
     * Sparse AD8088 motherboard-RAM writes are different: the IIe DRAM
     * captures them at CAS, before the ordinary card-response window opens.
     * wr_dma_data_en therefore arms a dedicated window entirely inside PH0.
     * It shares the final LUT override input with the II+ read hold, leaving
     * every existing data-enable truth-table entry unchanged when inactive. */
    // Address and R/W drive only while an arbiter client explicitly requests
    // ownership; otherwise both buses remain tri-stated.
    wire apple_addr_rw_enable = ab_write.wr_addr_rw_en &&
        (ADDR_OWNER_PREISOLATED || !physical_bus_isolate);

    /* Card responses settle many fabric clocks before TAP_DATA_EMIT. Register
     * the physical data tuple so the 12-client arbiter does not remain on the
     * data or direction-pin paths. Address ownership, control lines,
     * and the early motherboard-RAM DMA write-window control stay live. DMA
     * clients hold their registered byte long before that window opens, so
     * the byte can share this stage. Keep the ownership metadata aligned with
     * the byte for the II+ read-hold test. */
    logic       physical_data_en_q;
    logic [7:0] physical_data_q = 8'h00;
    logic       physical_addr_rw_en_q;
    logic       physical_rw_q;
    logic       physical_inh_dependent_q;
    always_ff @(posedge clk) begin
        /* The cleared enable masks this byte during reset, so the data flops
         * need no reset input or extra control set. */
        physical_data_q <= ab_write.wr_data;
        if (!rstn) begin
            physical_data_en_q    <= 1'b0;
            physical_addr_rw_en_q <= 1'b0;
            physical_rw_q         <= 1'b1;
            physical_inh_dependent_q <= 1'b0;
        end else begin
            physical_data_en_q    <= ab_write.wr_data_en;
            physical_addr_rw_en_q <= ab_write.wr_addr_rw_en;
            physical_rw_q         <= ab_write.wr_rw;
            physical_inh_dependent_q <= ab_write.assert_inh;
        end
    end

    /* machine_inh_allowed may drop after this stage captures an INH-backed
     * response. Release that response with INH so motherboard RAM cannot
     * contend with the card for the rest of the Apple cycle. */
    wire physical_data_en_safe =
        physical_data_en_q &&
        (!physical_inh_dependent_q || inh_allowed);
    wire drive_live = bus_emit_state && physical_data_en_safe;
    wire read_response_live =
        drive_live && (!physical_addr_rw_en_q || physical_rw_q);
    localparam int DMA_WRITE_START_CLKS = 18;
    localparam int DMA_WRITE_END_CLKS   = 40;

    logic       data_override_q;
    logic       data_override_saved_q;
    logic [7:0] iiplus_read_data_q;
    logic       iiplus_read_inh_dependent_q;
    logic       phi0_clean_q;
    logic [5:0] dma_write_phase_q;

    wire phi0_clean_rise = phi0_clean && !phi0_clean_q;
    wire dma_write_request = ab_write.wr_dma_data_en &&
                             apple_addr_rw_enable && !ab_write.wr_rw;

    /* At 133.333 MHz, the IOB + two synchronizer flops make phi0_clean_rise
     * visible 22.5..30 ns after the slot edge. Eighteen more clocks plus the
     * output path put data at the slot about 159..174 ns after PH0 rises:
     * later than the 155 ns bus-turnaround floor and before the 210 ns CAS
     * setup deadline. Releasing at clock 40 lands around 333..345 ns, safely
     * after CAS+55 ns and well before PH0 falls, including the long cycle.
     *
     * The same final-LUT input retains the established II+ saved-byte read
     * hold. The two uses cannot overlap: a DMA write owns address/R-W while a
     * slot read response is generated only for the motherboard CPU. */
    always_ff @(posedge clk) begin
        if (!rstn) begin
            data_override_q       <= 1'b0;
            data_override_saved_q <= 1'b0;
            iiplus_read_data_q    <= 8'h00;
            iiplus_read_inh_dependent_q <= 1'b0;
            phi0_clean_q          <= 1'b0;
            dma_write_phase_q     <= 6'd0;
        end else begin
            phi0_clean_q <= phi0_clean;

            if (dma_write_request) begin
                data_override_saved_q <= 1'b0;
                iiplus_read_inh_dependent_q <= 1'b0;
                if (phi0_clean_rise) begin
                    dma_write_phase_q <= 6'd1;
                    data_override_q   <= 1'b0;
                end else if (dma_write_phase_q != 6'd0) begin
                    if (dma_write_phase_q ==
                        6'(DMA_WRITE_START_CLKS)) begin
                        data_override_q <= 1'b1;
                    end
                    if (dma_write_phase_q == 6'(DMA_WRITE_END_CLKS)) begin
                        dma_write_phase_q <= 6'd0;
                        data_override_q   <= 1'b0;
                    end else begin
                        dma_write_phase_q <= dma_write_phase_q + 6'd1;
                    end
                end else begin
                    data_override_q <= 1'b0;
                end
            end else if (dma_write_phase_q != 6'd0) begin
                // Cancellation or reset of the client request is fail-safe.
                dma_write_phase_q     <= 6'd0;
                data_override_q       <= 1'b0;
                data_override_saved_q <= 1'b0;
                iiplus_read_inh_dependent_q <= 1'b0;
            end else if (!host_is_iiplus) begin
                data_override_q       <= 1'b0;
                data_override_saved_q <= 1'b0;
                iiplus_read_inh_dependent_q <= 1'b0;
            end else if (phi0_fall) begin
                data_override_q       <= 1'b0;
                data_override_saved_q <= 1'b0;
                iiplus_read_inh_dependent_q <= 1'b0;
            end else if (read_response_live && phi0_filt) begin
                data_override_q       <= 1'b1;
                data_override_saved_q <= 1'b1;
                iiplus_read_data_q    <= physical_data_q;
                iiplus_read_inh_dependent_q <=
                    physical_inh_dependent_q;
            end
        end
    end
    wire iiplus_read_hold_allowed =
        !iiplus_read_inh_dependent_q || inh_allowed;
    wire iiplus_read_hold_active =
        host_is_iiplus && data_override_q && data_override_saved_q &&
        iiplus_read_hold_allowed;
    wire data_override_safe =
        data_override_q &&
        (!data_override_saved_q || iiplus_read_hold_allowed);

    /* The direction-output route is bounded in the XDC so the asynchronous
     * raw-PHI0 release remains placement-independent. Keep this one gate at
     * the timing-clean placement validated by the promoted checkpoint. An
     * unconstrained incremental run moved the equivalent inferred LUT seven
     * columns away and changed this path from +0.362 ns to -0.052 ns.
     *
     * Keep the bus phase and staged response enable as separate LUT inputs.
     * This keeps raw PHI0 as the release input of the placed LUT. INH safety
     * stays in a small pre-gate fed by the staged response tag.
     *
     * LUT inputs implement:
     *   (bus_emit_state && physical_data_en_safe &&
     *       (PHI0 || (!host_is_iiplus && addr_rw_enable))) ||
     *   data_override_safe
     * INIT bit order is {I5,I4,I3,I2,I1,I0}. */
    /* Fold the direct output kill into the two request inputs. Raw PHI0 then
     * reaches the direction pin through only the placed LUT and pad, while
     * isolation still removes both the normal and override drive terms in
     * the same combinational cycle. */
    wire bus_emit_state_active = bus_emit_state && !physical_bus_isolate;
    wire data_override_active  = data_override_safe && !physical_bus_isolate;
    wire apple_data_enable;
    (* LOC = "SLICE_X104Y23", BEL = "A6LUT", DONT_TOUCH = "TRUE" *)
    LUT6 #(.INIT(64'hFFFF_FFFF_8088_8080)) apple_data_enable_lut (
        .I0(bus_emit_state_active),
        .I1(physical_data_en_safe),
        .I2(apple_phi0_pin),
        .I3(host_is_iiplus),
        .I4(physical_addr_rw_en_q),
        .I5(data_override_active),
        .O(apple_data_enable)
    );
    wire [7:0] apple_data_out =
        iiplus_read_hold_active ? iiplus_read_data_q : physical_data_q;

    assign apple_data_pin = apple_data_enable ? apple_data_out : 8'hzz;
    assign apple_addr_pin = apple_addr_rw_enable ? ab_write.wr_addr : 16'hzzzz;
    assign apple_rw_pin   = apple_addr_rw_enable ? ab_write.wr_rw   : 1'hz;

    /* The production board has no dedicated IRQ assertion transistor: U19
     * (the planned A2CTRL.IRQ pin) is physically unconnected, so IRQ uses the
     * bidirectional TXS0108 lane. A II/II+ motherboard pulls IRQ up through
     * 1 kOhm. The TXS lane's weak static sink may not hold that net below a
     * valid NMOS-6502 low, although its falling-edge accelerator does.
     *
     * While a virtual card's level request remains asserted on a II+, hold the
     * pin low except for a short high-Z re-arm notch before the late-PHI0 CPU
     * sample. Reasserting at the old chirp's proven falling-edge point gives
     * the translator a fresh edge while the static low remains present for
     * almost the entire cycle. The //e keeps its hardware-validated continuous
     * level-low behavior, and ab_write.assert_irq remains a level for the vTW
     * core's internal interrupt path. Driving is still low/high-Z only,
     * preserving the open-collector electrical contract. */
    wire apple_irq_drive_low = !physical_bus_isolate &&
                               ab_write.assert_irq &&
                               (!host_is_iiplus || !irq_rearm_release_q);
    assign apple_irq_pin = apple_irq_drive_low ? 1'b0 : 1'bz;
    // INH must only flip in the addr-phase window, so it's registered
    assign apple_inh_pin = (!physical_bus_isolate &&
                            apple_inh_assert && inh_allowed)
                                                 ? 1'b0 : 1'bz;
    /* A2FPGA.RESET is the bidirectional observation lane through U234.
     * Never assert RESET through that translator: every Appletini-generated
     * reset is merged onto the populated A2CTRL.RESET transistor in
     * apple_top. Keeping this lane high-Z lets the same input path observe
     * both motherboard-originated and Appletini-originated RESET events. */
    assign apple_res_pin = 1'bz;
    // Appletini does not drive these motherboard control lines.
    assign apple_rdy_pin = 1'bz;
    assign apple_nmi_pin = 1'bz;
    /* DMA# is open-drain. A vTW session latched as II/II+ automatically
     * releases and reasserts that low briefly once per Apple cycle to refresh
     * the TXS edge accelerator. Other masters and every //e session retain
     * the established continuous-low path. */
    wire apple_dma_requested = !physical_bus_isolate &&
                               ab_write.assert_dma && inh_allowed;
    wire apple_dma_drive_low =
        apple_dma_requested &&
        !(iiplus_dma_refresh_active && dma_rearm_release_q);
    assign apple_dma_pin = apple_dma_drive_low ? 1'b0 : 1'bz;

    // Tini board glue. Keep the main translators enabled as an input-only
    // monitor during ONE//e. Isolation already releases every FPGA-side
    // driver and forces both direction pins low (Apple -> FPGA), while U533
    // has a board-level low direction strap. This preserves PHI0/7M/Q3
    // observation so Apple power-up can kill ONE//e on its first clock edge.
    assign tini_oe_pin       = tini_5v_pin;
    assign tini_data_dir_pin = apple_data_enable;
    // Tini board's addr direction follows our addr-drive enable.
    assign tini_addr_dir_pin = apple_addr_rw_enable;
    assign ab_read           = ab_read_r;

    // ------------------------------------------------------------------
    // Sequential
    // ------------------------------------------------------------------
    /* Phase-locked II+ IRQ re-arm notch. The request asserts immediately and
     * remains low. Once per Apple cycle it is released for four fabric clocks,
     * then reasserted early enough to be low at the raw-PHI0 CPU sample. */
    always_ff @(posedge clk) begin
        if (!rstn || !host_is_iiplus || !ab_write.assert_irq) begin
            irq_rearm_release_q <= 1'b0;
        end else if (data_pipe[TAP_IRQ_REARM_RELEASE]) begin
            irq_rearm_release_q <= 1'b1;
        end else if (data_pipe[TAP_IRQ_REARM_ASSERT]) begin
            irq_rearm_release_q <= 1'b0;
        end
    end

    /* Keep the refresh entirely dormant unless an active II+ vTW session owns
     * /DMA. The 30 ns high-Z notch is phase locked late in PHI0 and ends
     * before the next PHI1 drive point. */
    always_ff @(posedge clk) begin
        if (!rstn || !iiplus_dma_refresh_active ||
            !apple_dma_requested) begin
            dma_rearm_release_q <= 1'b0;
        end else if (data_pipe[TAP_DMA_REARM_RELEASE]) begin
            dma_rearm_release_q <= 1'b1;
        end else if (data_pipe[TAP_DMA_REARM_ASSERT]) begin
            dma_rearm_release_q <= 1'b0;
        end
    end

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
    //    one cycle = the input window was not stable immediately before
    //    the fixed tap-59 production sample. The last offender's address
    //    and both bytes are latched. Self-served cycles are excluded
    //    because this probe is intended to characterize the host driver.
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
    logic [15:0] dbg_ghost_cnt_q;
    logic [15:0] dbg_ghost_addr_q;

    assign dbg_bus_quality  = {dbg_ring_cnt_q, dbg_short_cnt_q};
    assign dbg_tap_mismatch = {dbg_mm_rd_q, dbg_mm_wr_q};
    assign dbg_strobe_anom  = {dbg_extra_cnt_q, dbg_miss_cnt_q};
    assign dbg_tap_last     = dbg_tap_last_q;
    assign dbg_ghost_write  = {dbg_ghost_cnt_q, dbg_ghost_addr_q};

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
            dbg_ghost_cnt_q  <= 16'd0;
            dbg_ghost_addr_q <= 16'd0;
        end else begin
            /* Ghost-write meter: R/W sampled low, at the production data
             * tap, in a cycle a bus master owns but does not drive. R/W
             * residue only decays downward on this bus, so a low sample
             * here covers every possible low at the earlier DRAM CAS
             * decision point. */
            if (data_phase_snap_bus && ab_write.assert_dma &&
                !ab_write.wr_addr_rw_en && !rw_clean) begin
                if (dbg_ghost_cnt_q != 16'hFFFF) begin
                    dbg_ghost_cnt_q <= dbg_ghost_cnt_q + 16'd1;
                end
                dbg_ghost_addr_q <= addr_clean;
            end
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
