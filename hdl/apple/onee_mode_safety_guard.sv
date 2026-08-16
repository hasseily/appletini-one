`timescale 1ns / 1ps

// Stand-alone ONE//e mode safety interlock.
//
// This block does not drive Apple-side pins. Gate virtual-machine activity with
// onee_enable_effective/force_outputs_off. Disable every physical Apple-edge
// output while physical_bus_isolate is asserted. Raw Apple inputs may set the
// sticky safety state at once, but never feed the broad virtual-machine muxes
// as combinational data paths.
//
// The local clock must run continuously while the board is powered. Set
// QUIET_CYCLES longer than the largest valid gap between Apple bus transitions.

module onee_mode_safety_guard #(
    parameter integer QUIET_CYCLES = 2048
) (
    input  wire        clk,
    input  wire        resetn,

    input  wire        manual_enable_request,

    input  wire        apple_power_present_raw,
    input  wire        apple_phi0_raw,
    input  wire        apple_7m_raw,
    input  wire        apple_q3_raw,
    input  wire        apple_m2sel_raw,
    input  wire        apple_m2b0_raw,
    input  wire        apple_devsel_n_raw,
    input  wire        apple_reset_n_raw,
    input  wire        apple_inh_n_raw,
    input  wire        apple_irq_n_raw,
    input  wire        apple_nmi_n_raw,
    input  wire        apple_rdy_n_raw,
    input  wire        apple_dma_n_raw,

    output wire        onee_enable_effective,
    output wire        force_outputs_off,
    output wire        physical_bus_isolate,
    output logic       physical_isolation_hold = 1'b0,
    output wire        apple_activity_now,
    output logic       apple_activity_lockout,
    output logic       reselect_armed,
    output logic       onee_selected,
    output wire        apple_activity_quiet,
    output logic [2:0] inhibit_reason
);

    localparam integer QUIET_COUNT_WIDTH =
        (QUIET_CYCLES < 2) ? 1 : $clog2(QUIET_CYCLES + 1);
    localparam logic [QUIET_COUNT_WIDTH-1:0] QUIET_LIMIT = QUIET_CYCLES;

    // inhibit_reason values. Keep zero as the only enabled state.
    localparam logic [2:0] INHIBIT_NONE              = 3'd0;
    localparam logic [2:0] INHIBIT_RESET             = 3'd1;
    localparam logic [2:0] INHIBIT_APPLE_POWER       = 3'd2;
    localparam logic [2:0] INHIBIT_APPLE_ACTIVITY    = 3'd3;
    localparam logic [2:0] INHIBIT_ACTIVITY_LOCKOUT  = 3'd4;
    localparam logic [2:0] INHIBIT_RESELECT_REQUIRED = 3'd5;
    localparam logic [2:0] INHIBIT_MANUAL_OFF        = 3'd6;

    // Raw level order: slot power, DMA#, RDY#, NMI#, IRQ#, RESET#, INH#,
    // DEVSEL#, M2B0, M2SEL, Q3, 7M, PHI0.
    // The reset value represents the normal idle levels. If an unpowered bus
    // settles elsewhere, that first difference is handled as activity and the
    // guard remains off until the signals settle and the user selects off/on.
    localparam logic [12:0] APPLE_IDLE_LEVELS = 13'b0111111100000;

    wire [12:0] apple_raw_levels = {
        apple_power_present_raw,
        apple_dma_n_raw,
        apple_rdy_n_raw,
        apple_nmi_n_raw,
        apple_irq_n_raw,
        apple_reset_n_raw,
        apple_inh_n_raw,
        apple_devsel_n_raw,
        apple_m2b0_raw,
        apple_m2sel_raw,
        apple_q3_raw,
        apple_7m_raw,
        apple_phi0_raw
    };

    (* ASYNC_REG = "TRUE" *) logic [12:0] apple_sync_meta;
    (* ASYNC_REG = "TRUE" *) logic [12:0] apple_sync_level;
    logic [12:0] apple_sync_previous;

    wire apple_activity_sampled =
        |(apple_sync_level ^ apple_sync_previous);
    wire apple_sampled_nonidle =
        |(apple_sync_level ^ APPLE_IDLE_LEVELS);
    wire apple_activity_synchronized =
        apple_activity_sampled || apple_sampled_nonidle;

    // A raw input differs from its synchronized copy as soon as it changes.
    // This direct path is the fail-safe kill while the synchronizer and sticky
    // lockout catch up.
    wire apple_input_transition_now =
        |(apple_raw_levels ^ apple_sync_level);

    // A stable asserted control or stopped-high clock is still evidence of a
    // connected Apple. Keep it as a continuous veto instead of allowing the
    // quiet timer to age a non-idle level into a valid reselect state.
    wire apple_nonidle_level =
        |(apple_raw_levels ^ APPLE_IDLE_LEVELS);

    // Slot power is a level-sensitive veto, not just another transition. A
    // powered Apple must never share connector outputs with ONE//e mode.
    assign apple_activity_now =
        apple_power_present_raw || apple_input_transition_now ||
        apple_nonidle_level;

    always_ff @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            apple_sync_meta     <= APPLE_IDLE_LEVELS;
            apple_sync_level    <= APPLE_IDLE_LEVELS;
            apple_sync_previous <= APPLE_IDLE_LEVELS;
        end else begin
            apple_sync_meta     <= apple_raw_levels;
            apple_sync_level    <= apple_sync_meta;
            apple_sync_previous <= apple_sync_level;
        end
    end

    logic [QUIET_COUNT_WIDTH-1:0] quiet_count;

    assign apple_activity_quiet = (quiet_count == QUIET_LIMIT);

    always_ff @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            quiet_count <= {QUIET_COUNT_WIDTH{1'b0}};
        end else if (apple_activity_synchronized) begin
            quiet_count <= {QUIET_COUNT_WIDTH{1'b0}};
        end else if (!apple_activity_quiet) begin
            quiet_count <= quiet_count + 1'b1;
        end
    end

    // Reset and any raw transition asynchronously set the sticky lockout.
    // Clearing remains synchronous and is allowed only after a quiet interval
    // while the manual selection is off.
    wire activity_lockout_set =
        ~resetn | apple_power_present_raw | apple_input_transition_now |
        apple_nonidle_level;
    wire activity_lockout_clear =
        apple_activity_quiet &&
        !manual_enable_request &&
        !apple_activity_synchronized;

    always_ff @(posedge clk or posedge activity_lockout_set) begin
        if (activity_lockout_set) begin
            apple_activity_lockout <= 1'b1;
        end else if (apple_activity_synchronized) begin
            apple_activity_lockout <= 1'b1;
        end else if (activity_lockout_clear) begin
            apple_activity_lockout <= 1'b0;
        end
    end

    // Physical connector isolation has a different reset contract from the
    // virtual-machine enable. Ordinary Appletini host mode starts connected.
    // Raising the manual request isolates at once. If Apple activity is then
    // seen, the hold keeps the connector isolated even after manual off until
    // the same quiet manual-off condition which clears the activity lockout.
    wire physical_isolation_set =
        apple_activity_lockout &&
        (manual_enable_request || onee_selected);

    always_ff @(posedge clk or posedge physical_isolation_set) begin
        if (physical_isolation_set)
            physical_isolation_hold <= 1'b1;
        else if (!resetn)
            physical_isolation_hold <= 1'b0;
        else if (activity_lockout_clear)
            physical_isolation_hold <= 1'b0;
    end

    // A request which remains on across an activity event cannot restart the
    // mode. The user must first leave the request off after the quiet interval,
    // then make a new on selection.
    always_ff @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            reselect_armed <= 1'b0;
            onee_selected  <= 1'b0;
        end else if (apple_activity_lockout ||
                     apple_activity_synchronized) begin
            reselect_armed <= 1'b0;
            onee_selected  <= 1'b0;
        end else if (!manual_enable_request) begin
            onee_selected <= 1'b0;
            if (apple_activity_quiet)
                reselect_armed <= 1'b1;
        end else if (reselect_armed) begin
            reselect_armed <= 1'b0;
            onee_selected  <= 1'b1;
        end
    end

    // Contain every immediate fail-off source at one flop. Its Q is the only
    // high-fanout virtual-machine enable, so raw slot pins cannot become data
    // paths through every emulated card. Any pulse can only clear the mode.
    // Re-entry stays synchronous and still requires the guard's selected state.
    logic onee_run_q = 1'b0;
    wire mode_kill_async =
        !resetn ||
        !manual_enable_request ||
        apple_activity_lockout;

    always_ff @(posedge clk or posedge mode_kill_async) begin
        if (mode_kill_async)
            onee_run_q <= 1'b0;
        else if (onee_selected)
            onee_run_q <= 1'b1;
    end

    assign onee_enable_effective = onee_run_q;
    assign force_outputs_off = !onee_run_q;

    // Active high means every physical Apple-edge driver must be disabled.
    // This signal intentionally stays high while ONE//e runs; it is not the
    // inverse of onee_enable_effective.
    assign physical_bus_isolate =
        manual_enable_request || onee_selected || physical_isolation_hold;

    always_comb begin
        if (!resetn)
            inhibit_reason = INHIBIT_RESET;
        else if (apple_power_present_raw)
            inhibit_reason = INHIBIT_APPLE_POWER;
        else if (apple_activity_now || apple_activity_sampled)
            inhibit_reason = INHIBIT_APPLE_ACTIVITY;
        else if (apple_activity_lockout)
            inhibit_reason = INHIBIT_ACTIVITY_LOCKOUT;
        else if (!manual_enable_request)
            inhibit_reason = INHIBIT_MANUAL_OFF;
        else if (!onee_selected)
            inhibit_reason = INHIBIT_RESELECT_REQUIRED;
        else
            inhibit_reason = INHIBIT_NONE;
    end

endmodule
