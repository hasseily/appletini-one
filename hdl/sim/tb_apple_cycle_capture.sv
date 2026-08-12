`timescale 1ns / 1ps

module tb_apple_cycle_capture;

    import apple_cycle_capture_pkg::*;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic            resetn = 1'b0;
    logic            soft_reset = 1'b0;
    globals::AppleBus_read ab_read = '0;
    globals::SoftSwitchState sss = '0;
    logic [8:0]      line_in_frame = '0;
    logic [6:0]      cycle_in_line = '0;
    logic            frame_en = 1'b0;
    logic            overlay_devsel_enabled = 1'b0;
    logic            overlay_capture_armed = 1'b0;
    logic            overlay_capture_bank_aux = 1'b0;
    logic [15:0]     overlay_capture_base = '0;
    logic [15:0]     overlay_capture_limit = '0;
    AppleCycleRecord cycle_capture_data;
    logic            cycle_capture_rd_en = 1'b0;
    logic            cycle_capture_empty;
    logic            capture_drop_sticky;
    logic            capture_drop_ack = 1'b0;
    logic            overlay_capture_drop_sticky;
    logic            shr_capture_active;

    int failures = 0;

    apple_cycle_capture dut (
        .clk(clk),
        .resetn(resetn),
        .soft_reset(soft_reset),
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
        .cycle_capture_data(cycle_capture_data),
        .cycle_capture_rd_en(cycle_capture_rd_en),
        .cycle_capture_empty(cycle_capture_empty),
        .capture_drop_sticky(capture_drop_sticky),
        .capture_drop_ack(capture_drop_ack),
        .overlay_capture_drop_sticky(overlay_capture_drop_sticky),
        .shr_capture_active(shr_capture_active)
    );

    task automatic check(input logic condition, input string label_text);
        if (condition !== 1'b1) begin
            $display("FAIL: %s", label_text);
            failures++;
        end
    endtask

    task automatic soft_reset_dut;
        begin
            @(negedge clk);
            soft_reset = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            soft_reset = 1'b0;
            ab_read = '0;
            sss = '0;
            frame_en = 1'b0;
            capture_drop_ack = 1'b0;
            overlay_devsel_enabled = 1'b0;
            overlay_capture_armed = 1'b0;
            overlay_capture_bank_aux = 1'b0;
            overlay_capture_base = '0;
            overlay_capture_limit = '0;
            @(posedge clk);
            #1;
        end
    endtask

    task automatic push_cycle;
        begin
            @(negedge clk);
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            ab_read.data_en = 1'b0;
            frame_en = 1'b0;
        end
    endtask

    task automatic pop_record(output AppleCycleRecord record_out);
        int guard;
        begin
            guard = 0;
            #1;
            while (cycle_capture_empty && (guard < 12)) begin
                @(posedge clk);
                #1;
                guard++;
            end
            if (cycle_capture_empty) begin
                $display("FAIL: timed out waiting for capture record");
                failures++;
                record_out = '0;
            end else begin
                record_out = cycle_capture_data;
                @(negedge clk);
                cycle_capture_rd_en = 1'b1;
                @(posedge clk);
                #1;
                @(negedge clk);
                cycle_capture_rd_en = 1'b0;
            end
        end
    endtask

    task automatic pulse_drop_ack;
        begin
            @(negedge clk);
            capture_drop_ack = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            capture_drop_ack = 1'b0;
        end
    endtask

    task automatic fill_capture_fifo;
        int i;
        begin
            ab_read.addr = 16'h0400;
            ab_read.rw = 1'b0;
            sss.addr_decode_late = 24'h000400;
            sss.addr_decode_late_en = 1'b1;
            frame_en = 1'b0;
            @(negedge clk);
            ab_read.data_en = 1'b1;
            for (i = 0; i < 4096; i++) begin
                ab_read.data = i[7:0];
                @(posedge clk);
                #1;
                @(negedge clk);
            end
            ab_read.data_en = 1'b0;
        end
    endtask

    task automatic test_watched_write;
        AppleCycleRecord got;
        begin
            $display("TEST: watched video write");
            soft_reset_dut();
            ab_read.addr = 16'h0400;
            ab_read.rw = 1'b0;
            ab_read.data = 8'h5A;
            sss.addr_decode_late = 24'h000400;
            sss.addr_decode_late_en = 1'b1;
            push_cycle();
            pop_record(got);
            check(got.record_kind == RECORD_KIND_LEGACY,
                  "watched write kind");
            check(!got.frame_en, "watched write has no frame half");
            check(got.addr_decode_en && got.addr_decode == 24'h000400,
                  "watched write address");
            check(got.data == 8'h5A, "watched write data");
            #1 check(cycle_capture_empty, "watched write makes one record");
        end
    endtask

    task automatic test_frame_and_write;
        AppleCycleRecord got;
        begin
            $display("TEST: combined frame and video write");
            soft_reset_dut();
            ab_read.addr = 16'h2345;
            ab_read.rw = 1'b0;
            ab_read.data = 8'hA7;
            sss.addr_decode_late = 24'h012345;
            sss.addr_decode_late_en = 1'b1;
            sss.sw_80store = 1'b1;
            sss.sw_ramwrt = 1'b1;
            sss.sw_text = 1'b1;
            sss.sw_page2 = 1'b1;
            sss.sw_altcharset = 1'b1;
            sss.sw_dhires = 1'b1;
            line_in_frame = 9'd123;
            cycle_in_line = 7'd45;
            frame_en = 1'b1;
            push_cycle();
            pop_record(got);
            check(got.record_kind == RECORD_KIND_LEGACY,
                  "combined record kind");
            check(got.frame_en && got.line_in_frame == 9'd123 &&
                  got.cycle_in_line == 7'd45,
                  "combined record frame position");
            check(got.sw_80store && got.sw_ramwrt && got.sw_text &&
                  got.sw_page2 && got.sw_altcharset && got.sw_dhires,
                  "combined record switch state");
            check(got.addr_decode_en && got.addr_decode == 24'h012345 &&
                  got.data == 8'hA7,
                  "combined record write half");
            #1 check(cycle_capture_empty, "combined event makes one record");
        end
    endtask

    task automatic test_io_apple_order;
        AppleCycleRecord first;
        AppleCycleRecord second;
        begin
            $display("TEST: simultaneous IO and Apple record order");
            soft_reset_dut();
            ab_read.addr = 16'hC022;
            ab_read.rw = 1'b0;
            ab_read.data = 8'h39;
            sss.addr_decode_late_en = 1'b0;
            line_in_frame = 9'd17;
            cycle_in_line = 7'd61;
            frame_en = 1'b1;
            push_cycle();
            pop_record(first);
            pop_record(second);
            check(first == pack_io_write_record(16'hC022, 8'h39, 9'd17,
                                                7'd61),
                  "IO record comes first with packed fields");
            check(second.record_kind == RECORD_KIND_LEGACY &&
                  second.frame_en && second.line_in_frame == 9'd17 &&
                  second.cycle_in_line == 7'd61,
                  "Apple frame record comes second");
            #1 check(cycle_capture_empty,
                     "simultaneous event makes exactly two records");
        end
    endtask

    task automatic test_pending_drop_set_wins;
        AppleCycleRecord first;
        AppleCycleRecord second;
        begin
            $display("TEST: pending collision, set-wins, and ack");
            soft_reset_dut();
            ab_read.addr = 16'hC022;
            ab_read.rw = 1'b0;
            ab_read.data = 8'h81;
            line_in_frame = 9'd8;
            cycle_in_line = 7'd9;
            frame_en = 1'b1;
            @(negedge clk);
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;

            // The pending Apple record writes now. This new write is the
            // third source and is dropped. Ack is high in the same cycle.
            @(negedge clk);
            ab_read.addr = 16'h0400;
            ab_read.data = 8'h82;
            sss.addr_decode_late = 24'h000400;
            sss.addr_decode_late_en = 1'b1;
            frame_en = 1'b0;
            capture_drop_ack = 1'b1;
            @(posedge clk);
            #1;
            check(capture_drop_sticky,
                  "pending collision sets sticky over simultaneous ack");
            check(!overlay_capture_drop_sticky,
                  "non-overlay pending collision leaves overlay clean");
            @(negedge clk);
            ab_read.data_en = 1'b0;
            capture_drop_ack = 1'b0;

            pop_record(first);
            pop_record(second);
            check(first.record_kind == RECORD_KIND_IO_WRITE,
                  "pending collision keeps first IO record");
            check(second.record_kind == RECORD_KIND_LEGACY && second.frame_en,
                  "pending collision keeps queued Apple record");
            #1 check(cycle_capture_empty,
                     "third pending-collision record is dropped");
            pulse_drop_ack();
            #1 check(!capture_drop_sticky, "ack clears pending-drop sticky");
        end
    endtask

    task automatic test_full_and_overlay_drop;
        begin
            $display("TEST: full FIFO drop flags and ack priority");
            soft_reset_dut();
            fill_capture_fifo();
            #1 check(dut.fifo_full, "capture FIFO reaches full boundary");

            // A normal loss sets only the general sticky. Set must beat ack.
            capture_drop_ack = 1'b1;
            ab_read.data = 8'hE1;
            push_cycle();
            capture_drop_ack = 1'b0;
            check(capture_drop_sticky,
                  "full normal write sets general sticky over ack");
            check(!overlay_capture_drop_sticky,
                  "full normal write leaves overlay sticky clear");
            pulse_drop_ack();
            #1 check(!capture_drop_sticky, "ack clears full-drop sticky");

            // This range is outside normal video memory and qualifies only
            // because overlay capture is armed.
            overlay_capture_armed = 1'b1;
            overlay_capture_bank_aux = 1'b0;
            overlay_capture_base = 16'h1000;
            overlay_capture_limit = 16'h1100;
            sss.addr_decode_late = 24'h001000;
            ab_read.addr = 16'h1000;
            ab_read.data = 8'hE2;
            capture_drop_ack = 1'b1;
            push_cycle();
            capture_drop_ack = 1'b0;
            check(capture_drop_sticky,
                  "full overlay-only write sets general sticky");
            check(overlay_capture_drop_sticky,
                  "full overlay-only write sets overlay sticky over ack");
            sss.addr_decode_late = 24'h000000;
            sss.addr_decode_late_en = 1'b0;
            ab_read.rw = 1'b1;
            pulse_drop_ack();
            #1 check(!capture_drop_sticky &&
                     !overlay_capture_drop_sticky,
                     "ack clears both overlay loss flags");

            soft_reset_dut();
            check(cycle_capture_empty, "soft reset clears full FIFO");
        end
    endtask

    task automatic test_c029_and_soft_reset;
        AppleCycleRecord got;
        begin
            $display("TEST: C029 state, SHR marker, and soft reset");
            soft_reset_dut();
            ab_read.addr = 16'hC029;
            ab_read.rw = 1'b0;
            ab_read.data = 8'hC0;
            line_in_frame = 9'd4;
            cycle_in_line = 7'd7;
            push_cycle();
            check(shr_capture_active, "C029 C0 enters SHR capture");
            pop_record(got);
            check(got == pack_io_write_record(16'hC029, 8'hC0, 9'd4, 7'd7),
                  "C029 transition record");

            // SHR suppresses ordinary frame pulses.
            ab_read.addr = 16'hFFFF;
            ab_read.rw = 1'b1;
            sss.addr_decode_late_en = 1'b0;
            line_in_frame = 9'd1;
            cycle_in_line = 7'd2;
            frame_en = 1'b1;
            push_cycle();
            repeat (2) begin
                @(posedge clk);
                #1;
            end
            check(cycle_capture_empty, "SHR suppresses non-marker frame");

            // Line 0, cycle 0 remains the one frame marker.
            line_in_frame = 9'd0;
            cycle_in_line = 7'd0;
            frame_en = 1'b1;
            push_cycle();
            pop_record(got);
            check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en &&
                  got.line_in_frame == 9'd0 && got.cycle_in_line == 7'd0,
                  "SHR keeps line-zero frame marker");

            // Queue one record, then prove soft reset clears data and state.
            ab_read.addr = 16'h0400;
            ab_read.rw = 1'b0;
            ab_read.data = 8'h44;
            sss.addr_decode_late = 24'h000400;
            sss.addr_decode_late_en = 1'b1;
            push_cycle();
            check(!cycle_capture_empty, "record queued before soft reset");
            soft_reset_dut();
            check(cycle_capture_empty, "soft reset empties capture FIFO");
            check(!capture_drop_sticky && !overlay_capture_drop_sticky,
                  "soft reset clears drop flags");
            check(!shr_capture_active, "soft reset clears SHR state");
        end
    endtask

    initial begin
        repeat (4) @(posedge clk);
        @(negedge clk);
        resetn = 1'b1;
        repeat (2) begin
            @(posedge clk);
            #1;
        end

        test_watched_write();
        test_frame_and_write();
        test_io_apple_order();
        test_pending_drop_set_wins();
        test_full_and_overlay_drop();
        test_c029_and_soft_reset();

        if (failures == 0)
            $display("APPLE CYCLE CAPTURE PASS");
        else
            $display("APPLE CYCLE CAPTURE FAIL: %0d", failures);
        $finish;
    end

endmodule
