`timescale 1ns / 1ps

// Binary timer subset for the Apple-side calibration and counter accesses.
module pc_transporter_pit #(
    parameter int CLOCK_HZ = 133000000,
    parameter int TIMER_HZ = 1193182
)(
    input  logic       clk,
    input  logic       rstn,
    input  logic       read_strobe,
    input  logic       write_strobe,
    input  logic [1:0] read_addr,
    input  logic [1:0] write_addr,
    input  logic [7:0] wdata,
    output logic [7:0] rdata,
    input  logic       gate2,
    output logic       out2,
    output logic       irq0_tick
);
    localparam int PHASE_BITS = $clog2(CLOCK_HZ);
    localparam logic [PHASE_BITS-1:0] WRAP_THRESHOLD = PHASE_BITS'(CLOCK_HZ - TIMER_HZ);
    logic [PHASE_BITS-1:0] phase_q;
    wire phase_wrap = phase_q >= WRAP_THRESHOLD;
    logic tick_q;
    logic write_q;
    logic [1:0] write_addr_q;
    logic [7:0] wdata_q;

    logic [16:0] count_q [3];
    logic [16:0] reload_q [3];
    logic [15:0] latched_q [3];
    logic [7:0] write_low_q [3];
    logic [1:0] rw_q [3];
    logic [2:0] mode_q [3];
    logic [2:0] read_high_q, write_high_q, latch_valid_q;
    logic [2:0] loaded_q, run_q, out_q;
    logic [2:0] gate_prev_q;
    wire [2:0] gate = {gate2, 2'b11};

    assign out2 = out_q[2];
    assign irq0_tick = tick_q && run_q[0] && (count_q[0] == 17'd1) &&
        (mode_q[0] < 3'd3 || (mode_q[0] == 3'd3 && !out_q[0]));

    // Each counter's next read byte is registered. A read sees the count one
    // fabric clock late, which the asynchronous Apple read cannot detect. The
    // byte select and latch state change only at earlier bus commits.
    logic [7:0] read_byte_q [3];
    always_ff @(posedge clk) begin
        for (int n = 0; n < 3; n++) begin
            if (rw_q[n] == 2'd2 || read_high_q[n]) begin
                read_byte_q[n] <= latch_valid_q[n] ? latched_q[n][15:8] : count_q[n][15:8];
            end else begin
                read_byte_q[n] <= latch_valid_q[n] ? latched_q[n][7:0] : count_q[n][7:0];
            end
        end
    end
    assign rdata = (read_addr == 2'd3) ? 8'hff : read_byte_q[read_addr];

    for (genvar n = 0; n < 3; n++) begin : counters
        wire write_selected = write_q && write_addr_q == 2'(n);
        wire control = write_q && write_addr_q == 2'd3 && wdata_q[7:6] == 2'(n);
        wire load = write_selected &&
            (rw_q[n] != 2'd3 || write_high_q[n]);
        wire [15:0] load_word = rw_q[n] == 2'd1 ? {8'd0, wdata_q} :
            rw_q[n] == 2'd2 ? {wdata_q, 8'd0} : {wdata_q, write_low_q[n]};
        wire [16:0] load_count = load_word == 16'd0 ? 17'h10000 : {1'b0, load_word};

        always_ff @(posedge clk) begin
            if (!rstn) begin
                count_q[n] <= '0;
                reload_q[n] <= '0;
                latched_q[n] <= '0;
                write_low_q[n] <= '0;
                rw_q[n] <= 2'd3;
                mode_q[n] <= '0;
                read_high_q[n] <= 1'b0;
                write_high_q[n] <= 1'b0;
                latch_valid_q[n] <= 1'b0;
                loaded_q[n] <= 1'b0;
                run_q[n] <= 1'b0;
                out_q[n] <= 1'b0;
                gate_prev_q[n] <= 1'b0;
            end else begin
                gate_prev_q[n] <= gate[n];
                if (gate[n] && !gate_prev_q[n] && loaded_q[n] &&
                    (mode_q[n] == 3'd1 || mode_q[n] == 3'd5)) begin
                    count_q[n] <= reload_q[n];
                    out_q[n] <= mode_q[n] == 3'd5;
                    run_q[n] <= 1'b1;
                end else if (tick_q && run_q[n] &&
                    (gate[n] || mode_q[n] == 3'd1 || mode_q[n] == 3'd5)) begin
                    if (mode_q[n] == 3'd2) begin
                        out_q[n] <= count_q[n] != 17'd2;
                        count_q[n] <= count_q[n] == 17'd1 ? reload_q[n] : count_q[n] - 17'd1;
                    end else if (mode_q[n] == 3'd3) begin
                        if (count_q[n] == 17'd1) begin
                            out_q[n] <= !out_q[n];
                            count_q[n] <= (reload_q[n] >> 1) +
                                {16'd0, !out_q[n] && reload_q[n][0]};
                        end else begin
                            count_q[n] <= count_q[n] - 17'd1;
                        end
                    end else if (count_q[n] != 17'd0) begin
                        count_q[n] <= count_q[n] - 17'd1;
                        if (count_q[n] == 17'd1) begin
                            out_q[n] <= mode_q[n] < 3'd4;
                        end
                    end else begin
                        out_q[n] <= 1'b1;
                        run_q[n] <= 1'b0;
                    end
                end

                if (read_strobe && read_addr == 2'(n)) begin
                    if (rw_q[n] == 2'd3) begin
                        read_high_q[n] <= !read_high_q[n];
                    end
                    if (rw_q[n] != 2'd3 || read_high_q[n]) begin
                        latch_valid_q[n] <= 1'b0;
                    end
                end
                if (control) begin
                    if (wdata_q[5:4] == 2'd0) begin
                        if (!latch_valid_q[n]) begin
                            latched_q[n] <= count_q[n][15:0];
                            latch_valid_q[n] <= 1'b1;
                        end
                    end else begin
                        rw_q[n] <= wdata_q[5:4];
                        mode_q[n] <= wdata_q[3:1] >= 3'd6 ? {1'b0, wdata_q[2:1]} : wdata_q[3:1];
                        write_high_q[n] <= 1'b0;
                        read_high_q[n] <= 1'b0;
                        latch_valid_q[n] <= 1'b0;
                        loaded_q[n] <= 1'b0;
                        run_q[n] <= 1'b0;
                        out_q[n] <= wdata_q[3:1] != 3'd0;
                    end
                end
                if (write_selected && rw_q[n] == 2'd3) begin
                    write_high_q[n] <= !write_high_q[n];
                    if (!write_high_q[n]) begin
                        write_low_q[n] <= wdata_q;
                    end
                end
                if (load) begin
                    reload_q[n] <= load_count;
                    count_q[n] <= mode_q[n] == 3'd3 ? (load_count + 17'd1) >> 1 : load_count;
                    loaded_q[n] <= 1'b1;
                    run_q[n] <= mode_q[n] != 3'd1 && mode_q[n] != 3'd5;
                    out_q[n] <= mode_q[n] != 3'd0;
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        // The next Apple access follows many fabric clocks after commit.
        write_addr_q <= write_addr;
        wdata_q <= wdata;
        if (!rstn) begin
            phase_q <= '0;
            tick_q <= 1'b0;
            write_q <= 1'b0;
        end else begin
            phase_q <= phase_wrap ? phase_q - WRAP_THRESHOLD : phase_q + PHASE_BITS'(TIMER_HZ);
            tick_q <= phase_wrap;
            write_q <= write_strobe;
        end
    end
endmodule
