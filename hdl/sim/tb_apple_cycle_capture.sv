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
    logic            fake_shr_allowed = 1'b1;
    logic            native_gs_active = 1'b0;
    logic            native_gs_observe = 1'b0;
    logic            native_gs_tick = 1'b0;
    logic            native_gs_phase_locked = 1'b0;
    logic [7:0]      native_gs_c022 = 8'h00;
    logic [7:0]      native_gs_c029 = 8'h00;
    logic [7:0]      native_gs_c02b = 8'h00;
    logic [3:0]      native_gs_border = 4'h0;
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
    logic            direct_valid = 1'b0;
    logic [16:0]     direct_addr = '0;
    logic [7:0]      direct_data = '0;
    logic            direct_ready;
    logic            suppress_bus_writes = 1'b0;

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
        .fake_shr_allowed(fake_shr_allowed),
        .native_gs_active, .native_gs_observe, .native_gs_tick,
        .native_gs_phase_locked, .native_gs_c022, .native_gs_c029,
        .native_gs_c02b, .native_gs_border,
        .overlay_devsel_enabled(overlay_devsel_enabled),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(overlay_capture_bank_aux),
        .overlay_capture_base(overlay_capture_base),
        .overlay_capture_limit(overlay_capture_limit),
        .direct_valid(direct_valid),
        .direct_addr(direct_addr),
        .direct_data(direct_data),
        .direct_ready(direct_ready),
        .suppress_bus_writes(suppress_bus_writes),
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
            direct_valid = 1'b0;
            suppress_bus_writes = 1'b0;
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
            // Let the final assembled record reach the FIFO.
            @(posedge clk);
            #1;
            @(negedge clk);
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
            check(cycle_capture_empty,
                  "assembled record waits one cycle before FIFO write");
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

    task automatic test_back_to_back_writes;
        AppleCycleRecord got;
        int i;
        begin
            $display("TEST: back-to-back video write order");
            soft_reset_dut();
            ab_read.rw = 1'b0;
            sss.addr_decode_late_en = 1'b1;

            @(negedge clk);
            ab_read.data_en = 1'b1;
            for (i = 0; i < 3; i++) begin
                ab_read.addr = 16'h0400 + i[15:0];
                ab_read.data = 8'h70 + i[7:0];
                sss.addr_decode_late = 24'h000400 + i;
                @(posedge clk);
                #1;
                @(negedge clk);
            end
            ab_read.data_en = 1'b0;
            @(posedge clk);
            #1;

            for (i = 0; i < 3; i++) begin
                pop_record(got);
                check(got.record_kind == RECORD_KIND_LEGACY &&
                      got.addr_decode_en &&
                      got.addr_decode == (24'h000400 + i) &&
                      got.data == (8'h70 + i[7:0]),
                      "back-to-back write order and payload");
            end
            #1 check(cycle_capture_empty,
                     "back-to-back writes make three records");
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

            // The third record reaches arbitration one cycle later. Keep
            // ack high so the drop still tests set-over-clear priority.
            @(negedge clk);
            ab_read.data_en = 1'b0;
            frame_en = 1'b0;
            @(posedge clk);
            #1;
            check(capture_drop_sticky,
                  "pending collision sets sticky over simultaneous ack");
            check(!overlay_capture_drop_sticky,
                  "non-overlay pending collision leaves overlay clean");
            @(negedge clk);
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
            @(posedge clk);
            #1;
            check(capture_drop_sticky,
                  "full normal write sets general sticky over ack");
            check(!overlay_capture_drop_sticky,
                  "full normal write leaves overlay sticky clear");
            @(negedge clk);
            capture_drop_ack = 1'b0;
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

            // The address decode remains valid for most of a native cycle.
            // FIFO full outside data_en must not invent a lost overlay write.
            ab_read.data_en = 1'b0;
            repeat (2) begin
                @(posedge clk);
                #1;
            end
            check(!overlay_capture_drop_sticky,
                  "full FIFO without an overlay push leaves overlay clean");

            capture_drop_ack = 1'b1;
            push_cycle();
            @(posedge clk);
            #1;
            check(capture_drop_sticky,
                  "full overlay-only write sets general sticky");
            check(overlay_capture_drop_sticky,
                  "full overlay-only write sets overlay sticky over ack");
            @(negedge clk);
            capture_drop_ack = 1'b0;
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

            // UNKNOWN and IIgs physical hosts cannot enter the IIe-only
            // fake-SHR mode or send its C029 record to the PS renderer.
            fake_shr_allowed = 1'b0;
            push_cycle();
            check(!shr_capture_active,
                  "blocked C029 leaves fake-SHR state clear");
            check(cycle_capture_empty,
                  "blocked C029 does not emit a fake-SHR register record");

            fake_shr_allowed = 1'b1;
            push_cycle();
            check(shr_capture_active, "C029 C0 enters SHR capture");
            pop_record(got);
            check(got == pack_io_write_record(16'hC029, 8'hC0, 9'd4, 7'd7),
                  "C029 transition record");

            // A direct machine-policy change must clear a mode already set;
            // waiting for another C029 write would leave GS/fault state stale.
            @(negedge clk);
            fake_shr_allowed = 1'b0;
            @(posedge clk);
            #1;
            check(!shr_capture_active,
                  "permission loss clears an active fake-SHR mode");
            @(negedge clk);
            fake_shr_allowed = 1'b1;
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            ab_read.data_en = 1'b0;
            pop_record(got);
            check(shr_capture_active,
                  "permitted C029 can re-enter fake-SHR after policy return");

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
            @(posedge clk);
            #1;
            check(!cycle_capture_empty, "record queued before soft reset");
            soft_reset_dut();
            check(cycle_capture_empty, "soft reset empties capture FIFO");
            check(!capture_drop_sticky && !overlay_capture_drop_sticky,
                  "soft reset clears drop flags");
            check(!shr_capture_active, "soft reset clears SHR state");
        end
    endtask

    task automatic test_direct_video;
        AppleCycleRecord got;
        int accepted;
        soft_reset_dut();
        $display("TEST: TURBO direct ordering, mirror suppression, and backpressure");
        // Even a DATA edge with no physical record reserves the packing
        // stage. Readiness must depend on the strobe, not its late decode.
        @(negedge clk);
        ab_read.data_en = 1'b1;
        ab_read.addr = 16'h1000;
        ab_read.rw = 1'b1;
        frame_en = 1'b0;
        #1;
        check(!dut.io_push_request && !dut.apple_push_request,
              "plain DATA edge has no capture record");
        check(!direct_ready, "plain DATA edge reserves direct acceptance");
        @(posedge clk); #1;
        @(negedge clk); ab_read.data_en = 1'b0;
        #1;
        check(direct_ready, "plain DATA edge releases direct acceptance");
        check(cycle_capture_empty, "plain DATA edge queues no record");
        // A physical frame event is older than the direct byte offered on
        // that edge. It must survive the one-clock packing pipeline first.
        @(negedge clk);
        frame_en = 1'b1;
        ab_read.data_en = 1'b1;
        ab_read.rw = 1'b1;
        line_in_frame = 9'd12;
        direct_valid = 1'b1;
        direct_addr = 17'h12000;
        direct_data = 8'hA7;
        #1;
        check(!direct_ready, "direct cannot overtake raw physical frame");
        @(posedge clk); #1;
        @(negedge clk);
        frame_en = 1'b0;
        ab_read.data_en = 1'b0;
        while (!direct_ready) begin @(posedge clk); #1; end
        @(posedge clk); #1;
        @(negedge clk); direct_valid = 1'b0;
        pop_record(got);
        check(got.frame_en && got.line_in_frame == 9'd12,
              "physical frame precedes direct record");
        pop_record(got);
        check(got.addr_decode_en && got.addr_decode == 24'h012000 &&
              got.data == 8'hA7 && !got.frame_en, "direct AUX write record");

        // A delayed old motherboard copy must not overwrite the new byte
        // in the renderer shadow. Its contemporaneous frame half stays.
        suppress_bus_writes = 1'b1;
        ab_read.addr = 16'h2000;
        ab_read.rw = 1'b0;
        ab_read.data = 8'h11;
        sss.addr_decode_late = 24'h012000;
        sss.addr_decode_late_en = 1'b1;
        frame_en = 1'b1;
        push_cycle();
        pop_record(got);
        check(got.frame_en && !got.addr_decode_en,
              "delayed mirror loses memory half only");

        // Overlay commands remain ordered and visible during suppression.
        overlay_devsel_enabled = 1'b1;
        ab_read.addr = 16'hC0F3;
        ab_read.data = 8'h02;
        sss.addr_decode_late_en = 1'b0;
        push_cycle();
        pop_record(got);
        check(got.record_kind == RECORD_KIND_IO_WRITE,
              "overlay command remains visible in direct mode");

        soft_reset_dut();
        accepted = 0;
        @(negedge clk); direct_valid = 1'b1;
        repeat (4100) begin
            direct_addr = 17'h00400 + 17'(accepted);
            direct_data = 8'(accepted);
            if (direct_ready) accepted++;
            @(posedge clk); #1;
            @(negedge clk);
        end
        direct_valid = 1'b0;
        check(accepted == 4064, "direct records reserve 32 physical FIFO slots");
        check(!capture_drop_sticky, "direct backpressure does not drop records");
        for (int i = 0; i < accepted; i++) begin
            pop_record(got);
            check(got.addr_decode == 24'h000400 + 24'(i) && got.data == 8'(i),
                  "all accepted direct records retain order");
        end
        check(cycle_capture_empty, "all direct records drain exactly once");
    endtask

    task automatic test_native_gs_capture;
        AppleCycleRecord got;
        logic [63:0] bits;
        logic [15:0] controls [4];
        $display("TEST: native GS snapshots, timing, writes, and gaps");
        soft_reset_dut();
        native_gs_observe = 1'b1;
        ab_read.cycle_valid = 1'b1;
        ab_read.addr = 16'hC029;
        ab_read.data = 8'hC0;
        frame_en = 1'b1;
        native_gs_tick = 1'b1;
        direct_valid = 1'b1;
        push_cycle();
        native_gs_tick = 1'b0;
        check(!direct_ready, "pre-ID observation blocks direct legacy records");
        direct_valid = 1'b0;
        repeat (3) @(negedge clk);
        check(cycle_capture_empty && !shr_capture_active,
              "pre-ID observation emits no timing, IO, or synthetic SHR");

        ab_read.addr = 16'h4078;
        ab_read.data = 8'hA5;
        sss.addr_decode_late = 24'h014078;
        sss.addr_decode_late_en = 1'b1;
        frame_en = 1'b1;
        line_in_frame = 9'd5;
        cycle_in_line = 7'd17;
        native_gs_tick = 1'b1;
        push_cycle();
        native_gs_tick = 1'b0;
        pop_record(got);
        bits = got;
        check(bits == {3'd5, 28'd0, 24'h014078, 1'b1, 8'hA5} &&
              cycle_capture_empty,
              "pre-ID memory updates use only the untimed shadow half");
        ab_read.m2sel = 1'b1;
        frame_en = 1'b1;
        push_cycle();
        repeat (3) @(negedge clk);
        check(cycle_capture_empty, "pre-ID shadow ignores unqualified memory");
        ab_read.m2sel = 1'b0;
        ab_read.addr = 16'hC029;
        sss.addr_decode_late_en = 1'b0;

        native_gs_active = 1'b1;
        native_gs_phase_locked = 1'b1;
        native_gs_c022 = 8'hE2;
        native_gs_c029 = 8'h01;
        native_gs_c02b = 8'h10;
        native_gs_border = 4'hA;
        sss.sw_text = 1'b1;
        line_in_frame = 9'd0;
        cycle_in_line = 7'd0;
        frame_en = 1'b1;
        native_gs_tick = 1'b1;
        ab_read.data = 8'h80;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        ab_read.data_en = 1'b0;
        frame_en = 1'b0;
        pop_record(got);
        bits = got;
        check(bits == {3'd4, 1'b1, 9'd0, 7'd0, 11'h040,
                       8'hE2, 8'h01, 8'h10, 1'b1, 1'b1, 2'b00, 4'hA, 1'b0},
              "native entry snapshot has the agreed bit layout and precedes tick");
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en &&
              !got.addr_decode_en && got.line_in_frame == 0 &&
              got.cycle_in_line == 0, "native tick precedes simultaneous IO");
        pop_record(got);
        bits = got;
        check(bits == {3'd3, 16'hC029, 8'h80, 9'd0, 7'd0, 21'd0},
              "native C029 has its own IO kind and original bus byte");
        check(cycle_capture_empty && !capture_drop_sticky && !shr_capture_active,
              "three simultaneous native sources fit without synthetic SHR");

        // The host clock still advances through an invalid M2SEL cycle.
        cycle_in_line = 7'd1;
        native_gs_tick = 1'b1;
        ab_read.m2sel = 1'b1;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        ab_read.data_en = 1'b0;
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en &&
              got.cycle_in_line == 1 && !got.addr_decode_en && cycle_capture_empty,
              "an unqualified bus cycle produces only a beam tick");

        ab_read.m2sel = 1'b0;
        controls = '{16'hC022, 16'hC02B, 16'hC034, 16'hC068};
        foreach (controls[i]) begin
            ab_read.addr = controls[i];
            ab_read.data = 8'h40 + 8'(i);
            push_cycle();
            pop_record(got);
            bits = got;
            check(bits == {3'd3, controls[i], ab_read.data, 9'd0, 7'd1, 21'd0},
                  "native control write preserves address, byte, and timestamp");
        end
        ab_read.addr = 16'hC035;
        push_cycle();
        repeat (3) @(negedge clk);
        check(cycle_capture_empty, "native C035 must not emit a synthetic IO event");

        ab_read.addr = 16'hC02B;
        ab_read.data = 8'h00;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        native_gs_c02b = 8'h00;
        native_gs_phase_locked = 1'b0;
        pop_record(got);
        check(got.record_kind == RECORD_KIND_GS_IO,
              "standard change precedes its lock-loss snapshot");
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && !bits[8] &&
              bits[16:9] == 8'h00 && cycle_capture_empty,
              "lock loss publishes a snapshot even without a beam tick");
        native_gs_phase_locked = 1'b1;
        native_gs_tick = 1'b1;
        cycle_in_line = 7'd63;
        @(negedge clk);
        native_gs_tick = 1'b0;
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && bits[8] &&
              bits[50:44] == 7'd63, "reacquisition snapshots the corrected position");
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.cycle_in_line == 7'd63,
              "first corrected beam tick follows the snapshot");

        ab_read.addr = 16'h9FFF;
        ab_read.data = 8'hA6;
        native_gs_c029 = 8'h01;
        sss.addr_decode_late = 24'h019FFF;
        sss.addr_decode_late_en = 1'b1;
        cycle_in_line = 7'd2;
        push_cycle();
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en &&
              got.addr_decode_en && got.addr_decode == 24'h019FFF &&
              got.data == 8'hA6 && got.cycle_in_line == 2,
              "E1 SHR writes retain time while SHR and frame selection are off");

        ab_read.addr = 16'h0BFF;
        ab_read.data = 8'h3B;
        sss.addr_decode_late = 24'h000BFF;
        cycle_in_line = 7'd3;
        native_gs_tick = 1'b1;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        ab_read.data_en = 1'b0;
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en &&
              got.addr_decode_en && got.addr_decode == 24'h000BFF &&
              got.data == 8'h3B && got.cycle_in_line == 3 && cycle_capture_empty,
              "E0 text write and simultaneous tick share one ordered record");

        // An additional batch arriving while the older frame tick is pending
        // must signal a loss rather than interleave with that batch.
        cycle_in_line = 7'd0;
        native_gs_tick = 1'b1;
        @(negedge clk);
        cycle_in_line = 7'd1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && !bits[7],
              "periodic snapshot preserves unselected frame state");
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.cycle_in_line == 0 &&
              cycle_capture_empty && capture_drop_sticky,
              "older native batch remains ordered and new loss is explicit");
        pulse_drop_ack();

        cycle_in_line = 7'd2;
        native_gs_tick = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        soft_reset = 1'b1;
        @(negedge clk);
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.cycle_in_line == 2,
              "native reset does not discard the preceding queued beam tick");
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && bits[0],
              "native reset assertion emits a snapshot");
        soft_reset = 1'b0;
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && !bits[0],
              "native reset release emits a snapshot");

        // Lose the first exit snapshot behind a pending frame tick.
        // There is no later native frame to repeat the inactive state.
        cycle_in_line = 7'd0;
        native_gs_tick = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        native_gs_active = 1'b0;
        native_gs_observe = 1'b0;
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && bits[60],
              "last native frame snapshot precedes the exit");
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en,
              "last native frame tick precedes the exit");
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && !bits[60] &&
              capture_drop_sticky,
              "a dropped native exit retries its inactive snapshot");
        pulse_drop_ack();

        // A mode change while reset is held still has to retire the active
        // state, even if an older batch forces the first exit attempt out.
        native_gs_active = 1'b1;
        native_gs_observe = 1'b1;
        native_gs_tick = 1'b1;
        soft_reset = 1'b1;
        @(negedge clk);
        native_gs_tick = 1'b0;
        native_gs_active = 1'b0;
        native_gs_observe = 1'b0;
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && bits[60] && bits[0],
              "reset-held native entry is ordered");
        pop_record(got);
        check(got.record_kind == RECORD_KIND_LEGACY && got.frame_en,
              "reset-held entry preserves the queued tick");
        pop_record(got);
        bits = got;
        check(got.record_kind == RECORD_KIND_GS_STATE && !bits[60] &&
              bits[0] && capture_drop_sticky,
              "native exit retries without clearing the FIFO while reset stays low");
        soft_reset = 1'b0;
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
        test_back_to_back_writes();
        test_frame_and_write();
        test_io_apple_order();
        test_pending_drop_set_wins();
        test_full_and_overlay_drop();
        test_c029_and_soft_reset();
        test_direct_video();
        test_native_gs_capture();

        if (failures == 0)
            $display("APPLE CYCLE CAPTURE PASS");
        else
            $display("APPLE CYCLE CAPTURE FAIL: %0d", failures);
        $finish;
    end

endmodule
