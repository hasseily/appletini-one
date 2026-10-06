`timescale 1ns / 1ps

module tb_ssi263_source_control;
    logic clk = 1'b0;
    logic cold_resetn = 1'b0;
    logic phone_write = 1'b0;
    logic wr_sel0 = 1'b0, wr_sel1 = 1'b0, wr_sel2 = 1'b0;
    logic [3:0] duration_phase = 4'd0, tparm = 4'd0;
    logic latched_ctrl = 1'b0, ampct_zero = 1'b0;
    logic voice_amplitude_zero = 1'b0, fricative_amplitude_zero = 1'b0;
    logic tpho5 = 1'b0, u62_q_n = 1'b0, d3_or_d4 = 1'b0;
    logic phi1 = 1'b0, phi0_rise = 1'b0;
    wire pw0, pw1, pw2, pw3, pw5, u20, fric1_sw, fric2_sw;
    wire u104c, ampct0, fricative, u32b;
    wire [7:0] state_known;
    wire [3:0] logic_known;
    wire [7:0] state_value = {fric2_sw, fric1_sw, u20, pw5, pw3, pw2, pw1, pw0};
    wire [3:0] logic_value = {u32b, fricative, ampct0, u104c};
    integer vector_file, scanned, cases = 0;
    logic [55:0] vector;

    always #5 clk = ~clk;

    ssi263_source_control dut (.*);

    task automatic cycle(input logic [31:0] stimulus);
        @(negedge clk);
        phone_write = stimulus[0];
        wr_sel0 = stimulus[1];
        wr_sel1 = stimulus[2];
        wr_sel2 = stimulus[3];
        duration_phase = stimulus[7:4];
        tparm = stimulus[11:8];
        latched_ctrl = stimulus[12];
        ampct_zero = stimulus[13];
        voice_amplitude_zero = stimulus[14];
        fricative_amplitude_zero = stimulus[15];
        tpho5 = stimulus[16];
        u62_q_n = stimulus[17];
        d3_or_d4 = stimulus[18];
        phi1 = stimulus[19];
        phi0_rise = stimulus[20];
        cold_resetn = stimulus[21];
        // Check transparency before the next fabric edge as well. A plain
        // register that only samples when Phi1 is high would pass post-edge
        // vectors but would not implement the open U112 path.
        #1;
        if (cold_resetn && phi1 &&
            ((state_known[6] !== state_known[5]) ||
             (state_known[5] && (fric1_sw !== u20))))
            $fatal(1, "SOURCE FAIL: U112 is not transparent during Phi1");
        @(posedge clk);
        #1;
    endtask

    initial begin
        if ($test$plusargs("invalid_phone")) begin
            cycle(32'h000000);
            cycle(32'h200003);
            $fatal(1, "Missing phone/selector contract rejection");
        end else if ($test$plusargs("invalid_selectors")) begin
            cycle(32'h000000);
            cycle(32'h200006);
            $fatal(1, "Missing simultaneous selector contract rejection");
        end else if ($test$plusargs("invalid_phi0")) begin
            cycle(32'h000000);
            cycle(32'h200001); // phone clear
            cycle(32'h200064); // PW1 set at duration phase 6
            cycle(32'h308408); // Phi0 rise + enabled selector-2 U20 write
            $fatal(1, "Missing Phi0/U20 contract rejection");
        end else begin
            vector_file = $fopen("source_control_vectors.mem", "r");
            if (vector_file == 0)
                $fatal(1, "Cannot open source_control_vectors.mem");
            while (!$feof(vector_file)) begin
                scanned = $fscanf(vector_file, "%h\n", vector);
                if (scanned != 1)
                    $fatal(1, "Malformed source-control vector %0d", cases);
                cycle(vector[55:24]);
                if (state_known !== vector[15:8] ||
                    ((state_value ^ vector[23:16]) & vector[15:8]) !== 8'b0 ||
                    logic_known !== vector[3:0] ||
                    ((logic_value ^ vector[7:4]) & vector[3:0]) !== 4'b0)
                    $fatal(1,
                        "SOURCE FAIL vector=%0d input=%h state=%h/%h expected=%h/%h logic=%h/%h expected=%h/%h",
                        cases, vector[55:24], state_value, state_known,
                        vector[23:16], vector[15:8], logic_value, logic_known,
                        vector[7:4], vector[3:0]);
                cases = cases + 1;
            end
            $fclose(vector_file);
            $display("SSI263 SOURCE CONTROL PASS cases=%0d", cases);
            $finish;
        end
    end
endmodule
