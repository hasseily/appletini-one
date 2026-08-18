`timescale 1ns / 1ps

// Dynamic raw-track test for the Disk II WOZ path. This bench loads a WOZ
// track through the CPU0 register window and services the card's DDR line
// port. It then compares native and vTW read state, writes all eight bits of
// one byte, reads the byte back through the LSS, and repeats the write while
// the image is protected.
module tb_disk2_woz_rw;
    localparam logic [20:0] TRACK_BASE_LINE = 21'h0E0000;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #3.75 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    logic [20:0] mc_line_addr;
    logic mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0] mc_wstrb;
    logic mc_valid;
    logic mc_ready = 1'b1;
    logic [63:0] mc_rdata = 64'h0;
    logic mc_rvalid = 1'b0;
    logic mc_read_pending_q = 1'b0;
    logic [63:0] mc_read_data_q = 64'h0;

    logic vtw_active = 1'b0;
    logic vtw_req_valid = 1'b0;
    logic [3:0] vtw_req_addr = 4'h0;
    logic vtw_req_ready;
    logic vtw_resp_valid;
    logic [7:0] vtw_resp_rdata;
    logic vtw_cycle_tick = 1'b0;
    logic vtw_native_cycle_active = 1'b0;
    logic vtw_time_ready;
    logic vtw_write_timing_active;

    logic [63:0] model_seed_line0 = 64'h0;
    logic [63:0] model_seed_line1 = 64'h0;
    logic [63:0] model_line0 = 64'h0;
    logic [63:0] model_line1 = 64'h0;
    integer model_write_count = 0;
    logic [20:0] model_last_write_line = 21'h0;
    logic [7:0] model_last_write_strobe = 8'h0;

    disk2_card dut (
        .clk(clk), .rstn(rstn),
        .ab_read(ab_read), .rom_serve_en(1'b0),
        .sss(sss), .slot_assign(3'd6),
        .as_common(as_common), .as_client(axi),
        .mc_line_addr(mc_line_addr), .mc_rw(mc_rw),
        .mc_wdata(mc_wdata), .mc_wstrb(mc_wstrb),
        .mc_valid(mc_valid), .mc_ready(mc_ready),
        .mc_rdata(mc_rdata), .mc_rvalid(mc_rvalid),
        .ab_write(ab_write),
        .vtw_active(vtw_active),
        .vtw_req_valid(vtw_req_valid), .vtw_req_addr(vtw_req_addr),
        .vtw_req_ready(vtw_req_ready),
        .vtw_resp_valid(vtw_resp_valid), .vtw_resp_rdata(vtw_resp_rdata),
        .vtw_cycle_tick(vtw_cycle_tick),
        .vtw_native_cycle_active(vtw_native_cycle_active),
        .vtw_time_ready(vtw_time_ready),
        .vtw_write_timing_active(vtw_write_timing_active),
        .sound_spinning(), .sound_qtrack(), .sound_event(),
        .sound_seek_start_qtrack(), .sound_seek_distance()
    );

    // One-cycle read response and byte-strobed writes for the two lines that
    // the small test track can prefetch.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            mc_read_pending_q <= 1'b0;
            mc_read_data_q <= 64'h0;
            mc_rvalid <= 1'b0;
            mc_rdata <= 64'h0;
            model_line0 <= model_seed_line0;
            model_line1 <= model_seed_line1;
            model_write_count <= 0;
            model_last_write_line <= 21'h0;
            model_last_write_strobe <= 8'h0;
        end else begin
            mc_rvalid <= mc_read_pending_q;
            if (mc_read_pending_q)
                mc_rdata <= mc_read_data_q;
            mc_read_pending_q <= 1'b0;

            if (mc_valid && mc_ready) begin
                if (mc_rw) begin
                    mc_read_pending_q <= 1'b1;
                    case (mc_line_addr)
                    TRACK_BASE_LINE: mc_read_data_q <= model_line0;
                    TRACK_BASE_LINE + 21'd1: mc_read_data_q <= model_line1;
                    default: mc_read_data_q <= 64'h0;
                    endcase
                end else begin
                    if (mc_line_addr != TRACK_BASE_LINE)
                        $fatal(1, "FAIL: WOZ write used line %05X, expected %05X",
                               mc_line_addr, TRACK_BASE_LINE);
                    if (mc_wstrb != 8'h01)
                        $fatal(1, "FAIL: WOZ write used strobe %02X, expected 01",
                               mc_wstrb);
                    for (int lane = 0; lane < 8; ++lane) begin
                        if (mc_wstrb[lane])
                            model_line0[lane * 8 +: 8] <= mc_wdata[lane * 8 +: 8];
                    end
                    model_write_count <= model_write_count + 1;
                    model_last_write_line <= mc_line_addr;
                    model_last_write_strobe <= mc_wstrb;
                end
            end
        end
    end

    task automatic check(input bit cond, input string msg);
        if (!cond)
            $fatal(1, "FAIL: %s", msg);
    endtask

    task automatic axi_write(input logic [7:0] reg_addr,
                             input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = reg_addr;
        as_common.wdata = value;
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        axi.awvalid = 1'b0;
        repeat (2) @(negedge clk);
    endtask

    // Perform one real Apple slot-I/O cycle. vtw_native_cycle_active selects
    // the physical sss_en source when the accelerator owns the CPU.
    task automatic physical_access(input logic [3:0] io_addr,
                                   input logic is_read,
                                   input logic [7:0] write_data,
                                   output logic [7:0] read_data);
        @(negedge clk);
        ab_read.addr = {12'hC0E, io_addr};
        ab_read.rw = is_read;
        ab_read.data = write_data;
        ab_read.serve_en = is_read;
        ab_read.data_en = !is_read;
        ab_read.sss_en = 1'b1;
        vtw_native_cycle_active = vtw_active;
        @(posedge clk);
        #1;
        read_data = ab_write.wr_data;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.data_en = 1'b0;
        ab_read.sss_en = 1'b0;
        ab_read.rw = 1'b1;
        vtw_native_cycle_active = 1'b0;
    endtask

    task automatic physical_tick;
        @(negedge clk);
        ab_read.sss_en = 1'b1;
        vtw_native_cycle_active = vtw_active;
        @(negedge clk);
        ab_read.sss_en = 1'b0;
        vtw_native_cycle_active = 1'b0;
    endtask

    task automatic source_ticks(input integer count,
                                input bit use_virtual_source);
        repeat (count) begin
            @(negedge clk);
            if (use_virtual_source)
                vtw_cycle_tick = 1'b1;
            else
                ab_read.sss_en = 1'b1;
            @(negedge clk);
            vtw_cycle_tick = 1'b0;
            ab_read.sss_en = 1'b0;
        end
    endtask

    task automatic direct_read(input logic [3:0] io_addr,
                               output logic [7:0] value);
        integer timeout;
        @(negedge clk);
        vtw_req_addr = io_addr;
        vtw_req_valid = 1'b1;
        timeout = 0;
        while (!vtw_req_ready && timeout < 100) begin
            @(negedge clk);
            timeout++;
        end
        check(vtw_req_ready, "private WOZ read never became ready");
        @(negedge clk);
        vtw_req_valid = 1'b0;
        timeout = 0;
        while (!vtw_resp_valid && timeout < 20) begin
            @(negedge clk);
            timeout++;
        end
        check(vtw_resp_valid, "private WOZ read produced no response");
        value = vtw_resp_rdata;
    endtask

    task automatic wait_for_track_cache;
        integer timeout;
        timeout = 0;
        while (!(dut.active_drive_loaded && dut.stream_line_hit_q) &&
               timeout < 200) begin
            @(negedge clk);
            timeout++;
        end
        check(dut.active_drive_loaded, "WOZ track alias never became active");
        check(dut.stream_line_hit_q, "WOZ track line never entered the cache");
    endtask

    task automatic wait_for_writes(input integer count);
        integer timeout;
        timeout = 0;
        while (model_write_count < count && timeout < 200) begin
            @(negedge clk);
            timeout++;
        end
        check(model_write_count == count,
              $sformatf("DDR write count was %0d, expected %0d",
                        model_write_count, count));
    endtask

    task automatic reset_and_load(input bit accelerated,
                                  input bit read_only,
                                  input logic [63:0] line0);
        logic [7:0] unused;
        @(negedge clk);
        rstn = 1'b0;
        vtw_active = accelerated;
        vtw_req_valid = 1'b0;
        vtw_cycle_tick = 1'b0;
        vtw_native_cycle_active = 1'b0;
        ab_read.serve_en = 1'b0;
        ab_read.data_en = 1'b0;
        ab_read.sss_en = 1'b0;
        model_seed_line0 = line0;
        model_seed_line1 = line0;
        repeat (5) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);

        axi_write(8'h10, read_only ? 32'h0000_0003 : 32'h0000_0001);
        axi_write(8'h07, 32'd16);
        axi_write(8'h0E, 32'd128);
        axi_write(8'h0F, 32'd0);
        axi_write(8'h15, 32'd32);
        axi_write(8'h20, 32'h0000_0000);
        axi_write(8'h06, 32'h0000_0005);
        physical_access(4'h9, 1'b0, 8'h00, unused);
        wait_for_track_cache();
        check(dut.track_woz_q, "loaded track was not marked WOZ");
        check(dut.track_bit_count_q == 17'd128,
              "WOZ bit count did not load");
        check(dut.track_bit_timing_q == 8'd32,
              "WOZ bit timing did not load");
    endtask

    task automatic write_and_readback(input bit accelerated);
        logic [7:0] local_value;
        logic [7:0] expected_prefix;

        reset_and_load(accelerated, 1'b0, 64'h0000_0000_0000_0000);
        check(vtw_active == accelerated,
              "write/readback session selected the wrong timing mode");

        // Q6 high with Q7 low reports write protection. Q7 high and Q6 high
        // load A5; Q6 low starts shifting its raw bits to media.
        physical_access(4'hD, 1'b1, 8'h00, local_value);
        check(dut.q6_q && !dut.q7_q,
              "Q6-high write-protect state did not latch");
        check(dut.disk_latch_q == 8'h00,
              "writable WOZ did not report write enabled");
        physical_access(4'hC, 1'b1, 8'h00, local_value);
        axi_write(8'h0F, 32'd0);
        axi_write(8'h15, 32'd32);
        wait_for_track_cache();

        physical_access(4'hF, 1'b0, 8'h00, local_value);
        check(!dut.q6_q && dut.q7_q && vtw_write_timing_active,
              "Q7-high write mode did not select physical timing");
        check(dut.woz_bit_accum_q == 16'd8,
              "Q7 entry did not consume its physical cycle");
        if (accelerated) begin
            source_ticks(1, 1'b1);
            check(dut.woz_bit_accum_q == 16'd8 &&
                  dut.drive_bit_offset_q[0] == 17'd0,
                  "vTW tick advanced WOZ while Q7 forced physical timing");
        end
        physical_access(4'hD, 1'b0, 8'hA5, local_value);
        check(dut.q6_q && dut.q7_q && dut.woz_write_started_q &&
              dut.woz_shift_q == 8'hA5,
              "Q6/Q7 data-load state did not capture A5");
        physical_access(4'hC, 1'b0, 8'h00, local_value);
        check(!dut.q6_q && dut.q7_q,
              "Q6-low data-shift state did not latch");

        physical_tick();
        wait_for_writes(1);
        check(dut.drive_bit_offset_q[0] == 17'd1 &&
              dut.woz_bit_accum_q == 16'd0 &&
              dut.woz_shift_q == 8'h4A,
              "first WOZ write cell did not shift A5 bit 7");
        check(model_line0[7:0] == 8'h80,
              "first WOZ write did not set raw bit 0");

        for (int bit_count = 2; bit_count <= 8; ++bit_count) begin
            // Q7 always selects physical Disk II ticks, even in vTW mode.
            source_ticks(4, 1'b0);
            wait_for_writes(bit_count);
            expected_prefix = 8'hA5 & (8'hFF << (8 - bit_count));
            check(model_line0[7:0] == expected_prefix,
                  $sformatf("WOZ prefix after %0d bits was %02X, expected %02X",
                            bit_count, model_line0[7:0], expected_prefix));
            check(dut.drive_bit_offset_q[0] == bit_count,
                  $sformatf("WOZ bit offset after %0d writes was %0d",
                            bit_count, dut.drive_bit_offset_q[0]));
        end
        check(model_line0[7:0] == 8'hA5,
              "eight WOZ cells did not store A5");
        check(dut.stream_write_count_q == 32'd8 && dut.write_dirty_q,
              "WOZ writes did not set the exact write count and dirty flag");
        check(model_last_write_line == TRACK_BASE_LINE &&
              model_last_write_strobe == 8'h01,
              "WOZ write did not retain its line and byte strobe");

        // Leave write mode, clear the old latch through LoadWriteProtect,
        // rewind, and read the byte back through the mode's real read path.
        physical_access(4'hE, 1'b0, 8'h00, local_value);
        physical_access(4'hD, 1'b1, 8'h00, local_value);
        check(dut.disk_latch_q == 8'h00,
              "write-protect sense did not clear the old writable latch");
        physical_access(4'hC, 1'b1, 8'h00, local_value);
        axi_write(8'h0F, 32'd0);
        axi_write(8'h15, 32'd32);
        wait_for_track_cache();
        source_ticks(36, accelerated);
        check(dut.disk_latch_q == 8'hA5,
              $sformatf("WOZ readback latch was %02X, expected A5",
                        dut.disk_latch_q));
        if (accelerated)
            direct_read(4'hC, local_value);
        else
            physical_access(4'hC, 1'b1, 8'h00, local_value);
        check(local_value == 8'hA5,
              $sformatf("WOZ readback was %02X, expected A5", local_value));
    endtask

    task automatic run_head_step(input bit accelerated,
                                 output logic [7:0] qtrack,
                                 output logic [7:0] phase,
                                 output logic [3:0] magnets,
                                 output logic [3:0] event_code,
                                 output logic [7:0] seek_start,
                                 output logic [7:0] seek_distance);
        logic [7:0] unused;

        reset_and_load(accelerated, 1'b0, 64'hA5A5_A5A5_A5A5_A5A5);
        // Keep the raw stream valid across the move from qtrack 0 to 2.
        axi_write(8'h20, 32'h0000_0200);
        repeat (2) @(posedge clk);
        check(dut.active_drive_loaded,
              "WOZ alias did not cover the head-move range");

        // Energize phase 1 while the head rests at phase 0. The I/O access is
        // physical in both modes; the ten delayed controller ticks come from
        // the native or vTW time source selected by this session.
        physical_access(4'h3, 1'b0, 8'h00, unused);
        check(dut.step_pending_q && dut.step_delay_q == 4'd10 &&
              dut.phase_on_q == 4'h2,
              "phase-1 access did not arm the head step");
        source_ticks(10, accelerated);
        check(!dut.step_pending_q && dut.step_delay_q == 4'd0,
              "head step did not complete after ten controller ticks");
        check(dut.drive_qtrack_q[0] == 8'd2 && dut.drive_phase_q[0] == 8'd1,
              "WOZ head did not step from qtrack 0 to qtrack 2");
        check(dut.active_drive_loaded,
              "WOZ alias was lost after the head move");

        // The controller reports the registered step on the next fabric edge.
        @(posedge clk);
        #1;

        qtrack = dut.drive_qtrack_q[0];
        phase = dut.drive_phase_q[0];
        magnets = dut.phase_on_q;
        event_code = dut.sound_event_q;
        seek_start = dut.sound_seek_start_qtrack_q;
        seek_distance = dut.sound_seek_distance_q;
    endtask

    logic [16:0] native_offset;
    logic [15:0] native_accum;
    logic [7:0] native_latch;
    logic [7:0] native_shift;
    logic [3:0] native_head_window;
    logic [31:0] native_stream_reads;
    logic [7:0] value;
    logic [7:0] native_step_qtrack;
    logic [7:0] native_step_phase;
    logic [3:0] native_step_magnets;
    logic [3:0] native_step_event;
    logic [7:0] native_step_start;
    logic [7:0] native_step_distance;
    logic [7:0] vtw_step_qtrack;
    logic [7:0] vtw_step_phase;
    logic [3:0] vtw_step_magnets;
    logic [3:0] vtw_step_event;
    logic [7:0] vtw_step_start;
    logic [7:0] vtw_step_distance;

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        ab_read.cycle_valid = 1'b1;
        sss = '0;
        as_common = '0;
        axi.awvalid = 1'b0;

        // Native WOZ read: four 1 MHz ticks form one 32-unit bit cell. A
        // repeating A5 track reaches the data latch after nine bit cells.
        reset_and_load(1'b0, 1'b0, 64'hA5A5_A5A5_A5A5_A5A5);
        source_ticks(1, 1'b0);
        check(dut.woz_bit_accum_q == 16'd8 &&
              dut.drive_bit_offset_q[0] == 17'd0,
              "native first tick did not add eight timing units");
        source_ticks(2, 1'b0);
        check(dut.woz_bit_accum_q == 16'd24 &&
              dut.drive_bit_offset_q[0] == 17'd0,
              "native partial bit-cell timing changed");
        source_ticks(1, 1'b0);
        check(dut.woz_bit_accum_q == 16'd0 &&
              dut.drive_bit_offset_q[0] == 17'd1,
              "native fourth tick did not advance one raw bit");
        source_ticks(32, 1'b0);
        check(dut.drive_bit_offset_q[0] == 17'd9,
              "native raw-bit offset did not advance nine cells");
        check(dut.stream_read_count_q == 32'd9,
              "native raw-bit count did not record nine cells");
        check(dut.disk_latch_q == 8'hA5,
              $sformatf("native WOZ latch was %02X, expected A5",
                        dut.disk_latch_q));
        physical_access(4'hC, 1'b1, 8'h00, value);
        check(ab_write.wr_data_en && value == 8'hA5,
              $sformatf("native WOZ read returned %02X, expected A5", value));
        native_offset = dut.drive_bit_offset_q[0];
        native_accum = dut.woz_bit_accum_q;
        native_latch = dut.disk_latch_q;
        native_shift = dut.woz_shift_q;
        native_head_window = dut.drive_woz_head_window_q[0];
        native_stream_reads = dut.stream_read_count_q;

        // Repeat the same logical ticks through the vTW source and finish
        // with a private even-address read. Every controller state must match.
        reset_and_load(1'b1, 1'b0, 64'hA5A5_A5A5_A5A5_A5A5);
        source_ticks(36, 1'b1);
        direct_read(4'hC, value);
        check(value == 8'hA5,
              $sformatf("vTW WOZ read returned %02X, expected A5", value));
        check(dut.drive_bit_offset_q[0] == native_offset,
              "native and vTW raw-bit offsets differ");
        check(dut.woz_bit_accum_q == native_accum,
              "native and vTW bit accumulators differ");
        check(dut.disk_latch_q == native_latch,
              "native and vTW data latches differ");
        check(dut.woz_shift_q == native_shift,
              "native and vTW WOZ shift states differ");
        check(dut.drive_woz_head_window_q[0] == native_head_window,
              "native and vTW WOZ head windows differ");
        check(dut.stream_read_count_q == native_stream_reads,
              "native and vTW raw-cell counts differ");

        // Run the same eight-bit A5 media write and readback through each
        // mode. The native case keeps vtw_active low for the full session and
        // reads the result through the physical slot port.
        write_and_readback(1'b0);
        write_and_readback(1'b1);

        // A real phase change must also have the same delayed result when the
        // ten controller ticks come from native time or accelerated time.
        run_head_step(1'b0, native_step_qtrack, native_step_phase,
                      native_step_magnets, native_step_event,
                      native_step_start, native_step_distance);
        run_head_step(1'b1, vtw_step_qtrack, vtw_step_phase,
                      vtw_step_magnets, vtw_step_event,
                      vtw_step_start, vtw_step_distance);
        check(vtw_step_qtrack == native_step_qtrack &&
              vtw_step_phase == native_step_phase &&
              vtw_step_magnets == native_step_magnets,
              "native and vTW WOZ head-position states differ");
        check(vtw_step_event == native_step_event &&
              vtw_step_start == native_step_start &&
              vtw_step_distance == native_step_distance,
              "native and vTW WOZ seek reports differ");
        check(native_step_event == 4'd1 && native_step_start == 8'd0 &&
              native_step_distance == 8'd2,
              "WOZ outward head move reported the wrong seek event");

        // Protected WOZ. The raw clock and bit offset still move, but no DDR
        // write, write counter, or dirty indication may result.
        reset_and_load(1'b1, 1'b1, 64'h3C3C_3C3C_3C3C_3C3C);
        physical_access(4'hD, 1'b1, 8'h00, value);
        check(dut.disk_latch_q == 8'hFF,
              "protected WOZ did not report write protected");
        physical_access(4'hC, 1'b1, 8'h00, value);
        axi_write(8'h0F, 32'd0);
        axi_write(8'h15, 32'd32);
        wait_for_track_cache();
        physical_access(4'hF, 1'b0, 8'h00, value);
        physical_access(4'hD, 1'b0, 8'hFF, value);
        physical_access(4'hC, 1'b0, 8'h00, value);
        physical_tick();
        source_ticks(28, 1'b0);
        repeat (12) @(posedge clk);
        check(dut.drive_bit_offset_q[0] == 17'd8,
              "protected WOZ did not advance eight raw cells");
        check(model_write_count == 0 && model_line0[7:0] == 8'h3C,
              "protected WOZ changed DDR media");
        check(dut.stream_write_count_q == 32'd0 && !dut.write_dirty_q,
              "protected WOZ set write count or dirty state");

        $display("DISK2 WOZ RW PASS: native/vTW read state, native/vTW A5 write/readback, head-step equivalence, Q6/Q7, dirty count, and write protect");
        $finish;
    end
endmodule
