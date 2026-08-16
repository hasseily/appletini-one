`timescale 1ns / 1ps

module tb_onee_input_bridge;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic enabled = 1'b0;
    logic ps_wr_en = 1'b0;
    logic [7:0] ps_addr = 8'h00;
    logic [31:0] ps_wdata = 32'h0000_0000;
    logic [7:0] ps_read_addr = 8'h00;
    wire [31:0] ps_rdata;
    wire keyboard_event_valid;
    logic keyboard_event_ready = 1'b0;
    wire [6:0] keyboard_event_code;
    wire keyboard_any_down;
    wire [2:0] keyboard_modifiers;
    wire [2:0] pushbuttons;
    wire [31:0] paddle_values;
    wire warm_reset_request;
    logic warm_reset_ack = 1'b0;

    onee_input_bridge dut (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .ps_wr_en(ps_wr_en),
        .ps_addr(ps_addr),
        .ps_wdata(ps_wdata),
        .ps_read_addr(ps_read_addr),
        .ps_rdata(ps_rdata),
        .keyboard_event_valid(keyboard_event_valid),
        .keyboard_event_ready(keyboard_event_ready),
        .keyboard_event_code(keyboard_event_code),
        .keyboard_any_down(keyboard_any_down),
        .keyboard_modifiers(keyboard_modifiers),
        .pushbuttons(pushbuttons),
        .paddle_values(paddle_values),
        .warm_reset_request(warm_reset_request),
        .warm_reset_ack(warm_reset_ack)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic ps_write(
        input logic [7:0] addr,
        input logic [31:0] data
    );
        begin
            @(negedge clk);
            ps_addr = addr;
            ps_wdata = data;
            ps_wr_en = 1'b1;
            @(posedge clk);
            #1;
            ps_wr_en = 1'b0;
        end
    endtask

    task automatic accept_key(input logic [6:0] expected_code);
        begin
            check(keyboard_event_valid,
                  "key ready/valid source became empty early");
            check(keyboard_event_code == expected_code,
                  "key FIFO order or code changed");
            @(negedge clk);
            keyboard_event_ready = 1'b1;
            @(posedge clk);
            #1;
            keyboard_event_ready = 1'b0;
        end
    endtask

    integer key_index;
    logic [6:0] held_code;

    initial begin
        // Reset and disabled mode must expose no owned output or status.
        ps_read_addr = 8'h5F;
        repeat (3) @(posedge clk);
        #1;
        check(ps_rdata == 32'h0000_0000 &&
              !keyboard_event_valid && !keyboard_any_down &&
              keyboard_modifiers == 3'b000 && pushbuttons == 3'b000 &&
              paddle_values == 32'h0000_0000 && !warm_reset_request,
              "reset/disabled output mask failed");

        resetn = 1'b1;
        enabled = 1'b1;
        @(posedge clk);
        #1;
        check(paddle_values == 32'h8080_8080,
              "fresh enable did not start with neutral paddles");
        check(ps_rdata[31:24] == 8'hE1 && ps_rdata[9] &&
              ps_rdata[2] && !ps_rdata[0],
              "control status reset value is wrong");

        // Live state maps Apple keys into both modifier state and PB0/PB1.
        ps_write(8'h5D, 32'h0000_003F);
        check(keyboard_any_down &&
              keyboard_modifiers == 3'b011 &&
              pushbuttons == 3'b111,
              "live keyboard/button mapping failed");
        ps_read_addr = 8'h5D;
        #1;
        check(ps_rdata[5:0] == 6'h3F,
              "live input register did not read back raw state");

        ps_write(8'h5E, 32'hFE81_7F01);
        check(paddle_values == 32'hFE81_7F01,
              "four paddle bytes did not update together");
        ps_read_addr = 8'h5E;
        #1;
        check(ps_rdata == 32'hFE81_7F01,
              "paddle register did not read back");

        // A valid key must stay stable while ready is low.
        ps_write(8'h5C, 32'h0000_0041);
        held_code = keyboard_event_code;
        repeat (4) begin
            @(posedge clk);
            #1;
            check(keyboard_event_valid &&
                  keyboard_event_code == held_code && held_code == 7'h41,
                  "ready/valid key changed without acceptance");
        end
        accept_key(7'h41);
        check(!keyboard_event_valid,
              "accepted single key was duplicated");
        repeat (3) begin
            @(posedge clk);
            #1;
            check(!keyboard_event_valid,
                  "accepted key reappeared after ready dropped");
        end

        // Fill all eight entries. The ninth write drops and latches overflow.
        for (key_index = 0; key_index < 8;
             key_index = key_index + 1)
            ps_write(8'h5C, 32'h0000_0020 + key_index);
        ps_read_addr = 8'h5C;
        #1;
        check(ps_rdata[12] && ps_rdata[11:8] == 4'd8 &&
              ps_rdata[6:0] == 7'h20,
              "FIFO did not report its full boundary");
        ps_write(8'h5C, 32'h0000_0066);
        check(ps_rdata[14] && ps_rdata[11:8] == 4'd8,
              "full FIFO drop did not set sticky overflow");

        // Simultaneous pop and push at full must accept each event once and
        // keep the count at eight. The replacement appears after old entries.
        @(negedge clk);
        keyboard_event_ready = 1'b1;
        ps_addr = 8'h5C;
        ps_wdata = 32'h0000_0067;
        ps_wr_en = 1'b1;
        @(posedge clk);
        #1;
        keyboard_event_ready = 1'b0;
        ps_wr_en = 1'b0;
        check(ps_rdata[11:8] == 4'd8 &&
              keyboard_event_code == 7'h21,
              "full simultaneous pop/push changed count or duplicated head");
        for (key_index = 1; key_index < 8;
             key_index = key_index + 1)
            accept_key(7'h20 + key_index);
        accept_key(7'h67);
        check(!keyboard_event_valid,
              "replacement key was not consumed exactly once");

        // Overflow is sticky until its explicit clear command.
        ps_read_addr = 8'h5F;
        #1;
        check(ps_rdata[1], "overflow status did not stay sticky");
        ps_write(8'h5F, 32'h0000_0002);
        check(!ps_rdata[1], "overflow clear command failed");

        // Warm reset cannot clear before an acknowledge. A new request wins
        // over an acknowledge on the same clock, then clears on a later ack.
        ps_write(8'h5F, 32'h0000_0001);
        repeat (4) begin
            @(posedge clk);
            #1;
            check(warm_reset_request,
                  "warm reset request did not remain held");
        end
        @(negedge clk);
        warm_reset_ack = 1'b1;
        @(posedge clk);
        #1;
        warm_reset_ack = 1'b0;
        check(!warm_reset_request,
              "warm reset request did not clear on acknowledge");
        @(negedge clk);
        warm_reset_ack = 1'b1;
        ps_addr = 8'h5F;
        ps_wdata = 32'h0000_0001;
        ps_wr_en = 1'b1;
        @(posedge clk);
        #1;
        warm_reset_ack = 1'b0;
        ps_wr_en = 1'b0;
        check(warm_reset_request,
              "same-cycle reset request was lost to acknowledge");
        @(negedge clk);
        warm_reset_ack = 1'b1;
        @(posedge clk);
        #1;
        warm_reset_ack = 1'b0;
        check(!warm_reset_request,
              "second reset request did not clear on acknowledge");

        // Release command clears live state but leaves the FIFO contract
        // intact and restores centered paddles.
        ps_write(8'h5D, 32'h0000_003F);
        ps_write(8'h5E, 32'h0102_0304);
        ps_write(8'h5C, 32'h0000_0055);
        ps_write(8'h5F, 32'h0000_0008);
        check(!keyboard_any_down && keyboard_modifiers == 3'b000 &&
              pushbuttons == 3'b000 &&
              paddle_values == 32'h8080_8080 && keyboard_event_valid,
              "release-live command changed the wrong state");
        ps_write(8'h5F, 32'h0000_0004);
        check(!keyboard_event_valid,
              "FIFO flush command failed");

        // Drop enabled between clocks. Outputs and PS reads must mask in the
        // same delta cycle; the next edge must erase all saved state. Writes,
        // pops, and reset acks presented while disabled must do nothing.
        ps_write(8'h5D, 32'h0000_003F);
        ps_write(8'h5E, 32'h1122_3344);
        ps_write(8'h5C, 32'h0000_0042);
        ps_write(8'h5F, 32'h0000_0001);
        ps_read_addr = 8'h5F;
        @(negedge clk);
        #1;
        enabled = 1'b0;
        keyboard_event_ready = 1'b1;
        warm_reset_ack = 1'b1;
        ps_addr = 8'h5C;
        ps_wdata = 32'h0000_0061;
        ps_wr_en = 1'b1;
        #1;
        check(ps_rdata == 32'h0000_0000 &&
              !keyboard_event_valid && !keyboard_any_down &&
              keyboard_modifiers == 3'b000 && pushbuttons == 3'b000 &&
              paddle_values == 32'h0000_0000 && !warm_reset_request,
              "mid-transaction disable did not mask outputs at once");
        @(posedge clk);
        #1;
        ps_wr_en = 1'b0;
        keyboard_event_ready = 1'b0;
        warm_reset_ack = 1'b0;
        enabled = 1'b1;
        @(posedge clk);
        #1;
        check(!keyboard_event_valid && !keyboard_any_down &&
              keyboard_modifiers == 3'b000 && pushbuttons == 3'b000 &&
              paddle_values == 32'h8080_8080 && !warm_reset_request,
              "disabled clock did not clear all saved input state");
        ps_read_addr = 8'h5C;
        #1;
        check(ps_rdata[13] && !ps_rdata[14] &&
              ps_rdata[11:8] == 4'd0,
              "re-enabled FIFO retained stale or overflow state");

        $display("ONEE INPUT BRIDGE PASS");
        $finish;
    end

    initial begin
        #500000;
        $fatal(1, "ONEE INPUT BRIDGE TIMEOUT");
    end

endmodule
