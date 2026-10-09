`timescale 1ns / 1ps

// Convert the physical Apple Q3 clock into a one-cycle fabric enable.
//
// The Phasor feeds Q3 to both SSI-263AP XCK pins. Each socket has DIV2
// asserted, so the SSI control code consumes every second enable. Keeping Q3
// as a clock enable avoids creating another FPGA clock domain. Qualify the
// pin level before counting edges so short bus-drive glitches do not clock
// the speech engine or its FF divider.
module ssi263_xck_ce (
    input  logic clk,
    input  logic rstn,
    input  logic q3_raw,
    output logic xck_ce
);

    (* ASYNC_REG = "TRUE" *) logic q3_sync1_q;
    (* ASYNC_REG = "TRUE" *) logic q3_sync2_q;
    logic [7:0] q3_history_q;
    logic q3_filtered_q, q3_filtered_d_q;
    logic rise_pending_q;
    logic [5:0] spacing_q;

    // Eight equal samples reject short opposite-level pulses. At the
    // 133.333 MHz fabric clock this is 60 ns, well below the Apple Q3
    // phases (about 280 ns high / 210 ns low).
    wire q3_rise = q3_filtered_q && !q3_filtered_d_q;

    // A glitch near a real edge can delay qualification. Do not let the
    // next clean edge compress the native engine's work interval: it needs
    // up to 121 clocks. At least 62 clocks per Q3 enable gives DIV2 at least
    // 124 clocks. Nominal Q3 periods are about 65 clocks and never wait.
    // Hold an early qualified rise until ready; do not discard that clock.
    localparam logic [5:0] MIN_SPACING_MINUS_ONE = 6'd61;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            q3_sync1_q    <= 1'b0;
            q3_sync2_q    <= 1'b0;
            q3_history_q  <= 8'd0;
            q3_filtered_q <= 1'b0;
            q3_filtered_d_q <= 1'b0;
            rise_pending_q <= 1'b0;
            spacing_q     <= 6'd0;
            xck_ce        <= 1'b0;
        end else begin
            q3_sync1_q <= q3_raw;
            q3_sync2_q <= q3_sync1_q;
            q3_history_q <= {q3_history_q[6:0], q3_sync2_q};
            if (&q3_history_q) q3_filtered_q <= 1'b1;
            else if (~|q3_history_q) q3_filtered_q <= 1'b0;
            q3_filtered_d_q <= q3_filtered_q;

            xck_ce <= 1'b0;
            if (spacing_q != 6'd0) spacing_q <= spacing_q - 6'd1;
            if (q3_rise) rise_pending_q <= 1'b1;
            if ((q3_rise || rise_pending_q) && spacing_q == 6'd0) begin
                xck_ce <= 1'b1;
                spacing_q <= MIN_SPACING_MINUS_ONE;
                rise_pending_q <= 1'b0;
            end
        end
    end

endmodule
