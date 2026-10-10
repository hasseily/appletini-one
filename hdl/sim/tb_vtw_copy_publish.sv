`timescale 1ns / 1ps

// Production engine, shadow, capture arbitration and capture FIFO control.
// Only the Xilinx FIFO storage primitive uses its functional test model.
module tb_vtw_copy_publish #(parameter bit RUN_TESTS = 1'b1);
    import apple_cycle_capture_pkg::*;
    tb_vtw_copy_engine #(.RUN_TESTS(1'b0)) base();
    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    logic transport = 1'b1, soft_reset = 1'b0;
    logic cpu_valid = 1'b0;
    logic [16:0] cpu_addr = 17'h00400;
    logic [7:0] cpu_data = 8'h5A;
    wire cpu_ready, direct_valid, direct_ready, copy_ready;
    wire [16:0] direct_addr;
    wire [7:0] direct_data;
    wire active, empty, dropped, overlay_dropped;
    logic consume = 1'b1;
    integer consumer_period = 1;
    logic frame_en = 1'b0;
    wire rd_en = consume && (base.clocks % consumer_period == 0);
    AppleCycleRecord record;
    integer accepted = 0, received = 0, command_accepted = 0;
    integer cpu_records = 0, frame_records = 0, io_records = 0;
    integer stalls = 0, cases = 0;
    logic was_blocked = 1'b0;
    logic [16:0] blocked_addr;
    logic [7:0] blocked_data;
    logic [16:0] expected_addr [0:65535];
    logic [7:0] expected_data [0:65535];

    always_comb begin
        base.publish_active = active && transport && !soft_reset && base.rstn;
        base.publish_ready = copy_ready;
    end
    vtw_video_capture_mux mux (
        .enabled(transport && !soft_reset && base.rstn),
        .cpu_valid(cpu_valid), .cpu_addr(cpu_addr), .cpu_data(cpu_data),
        .cpu_ready(cpu_ready), .copy_valid(base.publish_valid),
        .copy_addr(base.publish_addr), .copy_data(base.publish_data),
        .copy_ready(copy_ready), .capture_valid(direct_valid),
        .capture_addr(direct_addr), .capture_data(direct_data),
        .capture_ready(direct_ready)
    );
    apple_cycle_capture capture (
        .clk(base.clk), .resetn(base.rstn), .soft_reset(soft_reset),
        .ab_read(ab_read), .sss(sss), .line_in_frame(9'd0),
        .cycle_in_line(7'd0), .frame_en(frame_en), .fake_shr_allowed(1'b1),
        .overlay_devsel_enabled(1'b0), .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0), .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0), .direct_valid(direct_valid),
        .direct_addr(direct_addr), .direct_data(direct_data),
        .direct_ready(direct_ready), .suppress_bus_writes(1'b1),
        .cycle_capture_data(record), .cycle_capture_rd_en(rd_en),
        .cycle_capture_empty(empty), .capture_drop_sticky(dropped),
        .capture_drop_ack(1'b0), .overlay_capture_drop_sticky(overlay_dropped),
        .shr_capture_active(active)
    );

    always @(posedge base.clk) if (base.rstn && !soft_reset) begin
        if (was_blocked && base.publish_valid)
            base.check(base.publish_addr == blocked_addr && base.publish_data == blocked_data,
                       "pending publication changed under backpressure");
        was_blocked = base.publish_valid && !copy_ready;
        blocked_addr = base.publish_addr;
        blocked_data = base.publish_data;
        if (base.publish_valid && !copy_ready) begin
            stalls++;
            base.check(!(base.sh_en && base.sh_we),
                       "blocked publication changed shadow without a capture record");
        end
        if (base.sh_en && base.sh_we && base.publish) begin
            base.check(base.publish_valid && copy_ready && !base.sh_word_we,
                       "publication shadow byte lacked the same-edge FIFO handshake");
            base.check(capture.fifo_wr_en && capture.record_din.addr_decode_en &&
                       capture.record_din.addr_decode == {7'd0, base.publish_addr} &&
                       capture.record_din.data == base.publish_data,
                       "publication did not enter the real ordered capture FIFO");
        end
        if (base.publish_valid && copy_ready) begin
            base.check(base.publish_addr == base.destination_at(command_accepted),
                       "capture addresses are not an ordered destination prefix");
            base.check(base.publish_data == (base.command_fill ? base.command_fill_data :
                       base.initial_byte(base.source_at(command_accepted))),
                       "capture data differs from the source byte");
            expected_addr[accepted] = base.publish_addr;
            expected_data[accepted] = base.publish_data;
            accepted++;
            command_accepted++;
        end
        if (rd_en && !empty) begin
            if (record.record_kind == RECORD_KIND_IO_WRITE)
                io_records++;
            else if (record.frame_en)
                frame_records++;
            else if (record.addr_decode_en && record.addr_decode == 24'h000400) begin
                base.check(record.data == cpu_data, "pending CPU record changed");
                cpu_records++;
            end else begin
                base.check(record.addr_decode_en && received < accepted,
                           "unexpected or duplicate capture record");
                base.check(record.addr_decode == {7'd0, expected_addr[received]} &&
                           record.data == expected_data[received],
                           "FIFO delivery lost or reordered a committed pixel");
                received++;
            end
        end
        base.check(!dropped && !overlay_dropped, "publication exhausted reserved capture space");
    end

    task automatic bus_write(input logic [15:0] address, input logic [7:0] value);
        ab_read.addr = address;
        ab_read.rw = 1'b0;
        ab_read.data = value;
        ab_read.data_en = 1'b1;
        base.tick();
        ab_read.data_en = 1'b0;
        repeat (3) base.tick();
    endtask
    task automatic drain;
        integer budget;
        consume = 1'b1;
        consumer_period = 1;
        budget = 10000;
        while ((!empty || received != accepted) && budget > 0) begin
            base.tick();
            budget--;
        end
        base.check(budget > 0 && received == accepted, "FIFO delivery did not drain");
    endtask
    task automatic fixture;
        drain();
        base.fixture();
        transport = 1'b1;
        soft_reset = 1'b0;
        base.publish = 1'b1;
        cpu_valid = 1'b0;
        command_accepted = 0;
        accepted = 0;
        received = 0;
        cpu_records = 0;
        frame_records = 0;
        io_records = 0;
        stalls = 0;
        if (!active) bus_write(16'hC029, 8'hC1);
        drain();
        io_records = 0;
        frame_records = 0;
    endtask
    task automatic success(input integer src, dst, count, input logic fill);
        integer started;
        fixture();
        started = base.clocks;
        base.launch(src, dst, count, fill);
        base.wait_done();
        base.check(!base.error && !base.aborted && base.completed == count,
                   "publish completion status");
        base.check(command_accepted == count && base.sh_wide_writes == 0,
                   "published byte count / shadow write width");
        if (src % 4 == 0 && dst % 4 == 0 && count % 4 == 0 && !fill && src < 'h20000)
            base.check(base.sh_reads == count / 4, "aligned source fetch lost 32-bit batching");
        base.verify_memory();
        drain();
        $display("PUBLISH MEASURE bytes=%0d fill=%0b source=%06X clocks=%0d stalls=%0d reads=%0d",
                 count, fill, src, base.clocks-started, stalls, base.sh_reads);
        cases++;
    endtask
    task automatic invalid(input integer src, dst, count);
        fixture();
        base.launch(src, dst, count, 1'b0);
        base.wait_done();
        base.check(base.error && !base.aborted && base.completed == 0,
                   "invalid publish range did not fail before memory access");
        base.check(base.sh_reads + base.sh_writes + base.ps_reads + base.ps_writes == 0 &&
                   command_accepted == 0, "invalid publish descriptor touched memory or capture");
        base.verify_memory();
        cases++;
    endtask

    initial if (RUN_TESTS) begin
        integer saved, budget, started;
        ab_read = '0;
        sss = '0;
        repeat (3) base.tick();
        base.rstn = 1'b1;
        base.tick();
        for (int ps = 0; ps < 2; ps++)
            for (int s = 0; s < 4; s++)
                for (int d = 0; d < 4; d++) begin
                    base.label = $sformatf("publish offsets source-space%0d %0d/%0d", ps,s,d);
                    success((ps ? 'h20200 : 'h0800)+s, 'h12000+d, 37, 1'b0);
                end
        base.label = "publish 128-byte row";
        success('h0800, 'h12010, 128, 1'b0);
        base.label = "publish 128-byte fill row";
        success(0, 'h12010, 128, 1'b1);
        base.label = "publish full pixel plane";
        success('h0800, 'h12000, 32000, 1'b0);
        base.label = "publish fill end of pixel plane";
        success(0, 'h19CF9, 7, 1'b1);
        base.label = "PRIVATE remains invisible to capture";
        fixture();
        base.publish=0;
        base.launch('h0800,'h12000,32,0);
        base.wait_done();
        base.verify_memory();
        drain();
        base.check(accepted==0 && base.completed==32 && base.sh_wide_writes==8,
                   "PRIVATE accidentally published pixels");
        cases++;
        base.label = "reject MAIN destination";
        invalid('h0800, 'h2000, 16);
        base.label = "reject AUX below pixel plane";
        invalid('h0800, 'h11FFF, 16);
        base.label = "reject SCB boundary crossing";
        invalid('h0800, 'h19CFF, 2);
        base.label = "reject SCB/palette destination";
        invalid('h0800, 'h19D00, 1);
        base.label = "reject extended AUX destination";
        invalid('h0800, 'h22000, 16);
        base.label = "reject overlapping publish";
        invalid('h12000, 'h12004, 16);
        base.label = "inactive captured mode rejects START";
        fixture();
        bus_write(16'hC029, 8'h80);
        base.launch('h0800, 'h12000, 32, 1'b0);
        base.wait_done();
        base.check(base.error && base.completed == 0 && command_accepted == 0,
                   "C029 bit7 alone enabled publish");
        cases++;
        base.label = "disabled egress rejects START";
        fixture();
        transport=0;
        base.launch('h0800,'h12000,32,0);
        base.wait_done();
        base.check(base.error && base.completed==0 && base.sh_reads==0 && base.sh_writes==0,
                   "disabled egress did not reject before memory access");
        transport=1;
        cases++;

        base.label = "FIFO reserves frame/IO slots under sustained backpressure";
        fixture();
        consume = 1'b0;
        base.launch('h0800, 'h12000, 5000, 1'b0);
        budget = 30000;
        while (!(base.publish_valid && !copy_ready) && budget > 0) begin base.tick(); budget--; end
        base.check(budget > 0 && command_accepted == 4064, "FIFO reservation watermark");
        saved = base.completed;
        repeat (50) base.tick();
        base.check(base.completed == saved && base.busy, "stalled publication advanced");
        bus_write(16'hC034, 8'h07);
        frame_en = 1'b1;
        bus_write(16'h1234, 8'h00);
        frame_en = 1'b0;
        base.check(!dropped && base.completed == saved, "reserved physical records were not protected");
        consume = 1'b1;
        consumer_period = 4;
        base.wait_done();
        base.check(base.completed == 5000 && !base.error && !base.aborted, "backpressure completion");
        base.verify_memory();
        drain();
        base.check(frame_records == 1 && io_records == 1, "reserved frame/IO record lost");
        cases++;

        base.label = "CPU record stays ahead of held-CPU publication";
        fixture();
        base.launch('h0800, 'h12000, 32, 1'b0);
        while (!base.publish_valid) base.tick();
        cpu_valid = 1'b1;
        saved = base.completed;
        base.tick();
        base.check(base.completed == saved, "copy overtook pending CPU record");
        cpu_valid = 1'b0;
        base.wait_done();
        drain();
        base.check(cpu_records == 1 && command_accepted == 32, "ingress arbitration lost a record");
        cases++;

        for (int cause = 0; cause < 4; cause++) begin
            base.label = $sformatf("partial-word cancel cause%0d", cause);
            fixture();
            base.launch('h0800, 'h12000, 32, 1'b0);
            while (base.completed != 2) base.tick();
            case (cause)
                0: base.abort_req = 1'b1;
                1: base.permit = 1'b0;
                2: transport = 1'b0;
                3: begin ab_read.addr=16'hC029;ab_read.data=0;ab_read.rw=0;ab_read.data_en=1;end
            endcase
            base.tick();
            ab_read.data_en=0;
            base.abort_req=0;base.permit=1;transport=1;
            base.wait_done();
            base.check(base.aborted && !base.error && base.completed == 2 && command_accepted == 2,
                       "cancel did not retain the exact atomic prefix");
            base.verify_memory();
            drain();
            cases++;
        end
        base.label = "cancel drains accepted PSRAM source read";
        fixture();
        base.launch('h20200, 'h12000, 32, 1'b0);
        base.await_request(1'b1,1'b1);
        base.abort_req=1;
        base.tick();
        base.check(base.busy && !base.done, "accepted source read was not drained");
        base.abort_req=0;
        base.wait_done();
        base.check(base.aborted && base.completed==0 && command_accepted==0,
                   "source cancellation published data");
        cases++;

        base.label = "hard reset suppresses a pending publish edge";
        fixture();
        base.launch('h0800,'h12000,32,0);
        while (!base.publish_valid) base.tick();
        base.track_progress=0;
        base.rstn=0;
        base.tick();
        base.check(!base.sh_en && !base.publish_valid && !base.busy && base.completed==0,
                   "reset committed a shadow byte or capture record");
        base.rstn=1;
        base.tick();
        base.check(command_accepted==0,"reset publication escaped");
        cases++;

        base.label = "200-row completed strip publication throughput";
        fixture();
        started=base.clocks;
        for (int row=0;row<200;row++) begin
            base.command_src='h0800+(row%8)*128;
            command_accepted=0;
            // Each descriptor has fresh progress, while the FIFO may hold
            // prior rows. Source staging is the real eight-row layout.
            base.written_bytes=0;
            base.launch(base.command_src,'h12010+row*160,128,0);
            base.wait_done();
            base.check(base.completed==128 && !base.error && !base.aborted,"row throughput status");
        end
        drain();
        base.check(accepted==25600 && received==25600,"frame byte count");
        $display("PUBLISH FRAME MEASURE bytes=25600 descriptors=200 clocks=%0d stalls=%0d",
                 base.clocks-started,stalls);
        $display("VTW COPY PUBLISH PASS cases=%0d checks=%0d",cases,base.checks);
        $finish;
    end
endmodule
