// Included in the production CPU/bus/shadow fixture by VTW_SHR_CAPTURE_TEST.
logic [16:0] shr_expected_addr [0:63];
logic [7:0] shr_expected_data [0:63];
integer shr_expected_count = 0, shr_observed_count = 0;
bit shr_check_records = 0;
always @(posedge clk) begin
    if (shr_check_records && rstn && video_record_valid && video_record_ready) begin
        check(shr_observed_count < shr_expected_count,
              "SHR capture emitted an extra record");
        check(video_record_addr === shr_expected_addr[shr_observed_count] &&
              video_record_data === shr_expected_data[shr_observed_count],
              $sformatf("SHR record %0d changed order/address/data", shr_observed_count));
        shr_observed_count++;
    end
end

task automatic shr_store(input logic [15:0] address, input logic [7:0] value,
                         input logic aux, input bit captured = 1);
    immediate(8'hA9, value);
    absolute(8'h8D, address);
    if (captured) begin
        shr_expected_addr[shr_expected_count] = {aux, address};
        shr_expected_data[shr_expected_count] = value;
        shr_expected_count++;
    end
endtask

task automatic shr_capture_case(input logic [1:0] mode,
                                input integer paging, input bit iiplus);
    integer guard, stopped_cycles, initial_posts;
    begin_program(mode);
    host_is_iiplus = iiplus;
    video_record_enable = 1;
    shr_check_records = 0;
    shr_expected_count = 0;
    shr_observed_count = 0;
    mb_ram[16'h4000] = 8'h11;
    mb_ram[16'h2000] = 8'h22;
    mb_ram[16'h0400] = 8'h17;
    mb_ram[16'h0800] = 8'h18;
    mb_aux[16'h0800] = 8'h19;
    mb_aux[16'h2000] = 8'h33;
    mb_aux[16'h9D00] = 8'h44;
    mb_aux[16'h9E00] = 8'h55;
    sh_write(18'h0A110, 8'h00);
    emit_hgr_mode();
    // This classic hidden-page byte predates SHR. C029 must still flush it.
    shr_store(16'h4000, 8'hA6, 0, mode == 2'd3);
    immediate(8'hA9, 8'hC1);
    absolute(8'h8D, 16'hC029);
    absolute(8'h8D, 16'hC005);
    if (paging == 1) shr_store(16'h9DF8, 8'h01, 1);
    if (paging == 2) post_main_wide = 1;
    shr_store(16'h2000, 8'h91, 1);
    shr_store(16'h9CFF, 8'h92, 1);
    shr_store(16'h9D00, 8'h93, 1);
    shr_store(16'h9DFB, 8'h94, 1);
    shr_store(16'h9E00, 8'h95, 1);
    shr_store(16'h9FFF, 8'h96, 1);
    // Intermediate values must reach capture, even at the same address.
    for (int i = 0; i < 8; i++) shr_store(16'h2000, 8'hA0 + 8'(i), 1);
    shr_store(16'h0800, 8'hA8, 1);
    absolute(8'h8D, 16'hC004);
    // Native ports use classic video windows as ordinary work memory while
    // SHR is displayed. Those writes must not force mirrors at every I/O.
    shr_store(16'h0400, 8'hB4, 0);
    shr_store(16'h0800, 8'hB8, 0);
    shr_store(16'h2000, 8'hB7, 0);
    shr_store(16'h4000, 8'hB9, 0);
    shr_store(16'h6000, 8'hBA, 0, paging != 0);
    // Ordinary non-posted RAM still has no renderer record.
    shr_store(16'hA000, 8'hBC, 0, 0);
    // Ordinary slot I/O, private-copy entry and bank switches do not create
    // retroactive SHR mirror traffic.
    absolute(8'hAD, 16'hC400);
    absolute(8'hAD, 16'hC0A0);
    absolute(8'hAD, 16'hC700);
    absolute(8'hAD, 16'hC020);
    absolute(8'h8D, 16'hA100);
    absolute(8'hAD, 16'hA110);
    immediate(8'hF0, 8'hFB);
    // Returning to classic graphics mirrors NEW writes normally. Old SHR
    // pixels remain private; the physical display must be redrawn.
    immediate(8'hA9, 8'h01);
    absolute(8'h8D, 16'hC029);
    absolute(8'h8D, 16'hC005);
    shr_store(16'h2000, 8'h7C, 1, mode == 2'd3);
    absolute(8'h8D, 16'hC004);
    absolute(8'hAD, 16'hC020);
    absolute(8'h8D, 16'hA101);
    halt_loop();
    shr_check_records = 1;
    start_program();
    // Block a SHR byte after its shadow commit. It must neither retire nor
    // enter the motherboard queue while the renderer cannot accept it.
    guard = 0;
    while (!(dut.xstate_q == dut.X_POST_STALL &&
             dut.cycle_addr_q == 16'h2000) && guard < 400000) begin
        @(negedge clk);
        guard++;
    end
    check(guard < 400000 && shr_capture_active &&
          dut.cycle_video_capture_only_q, "SHR byte did not select direct capture");
    video_record_ready = 0;
    stopped_cycles = cnt_core_cycles;
    initial_posts = cnt_posted_writes;
    repeat (16) begin
        @(posedge clk); #1ps;
        check(video_record_valid && video_record_addr == 17'h12000 &&
              video_record_data == 8'h91 && cnt_core_cycles == stopped_cycles &&
              cnt_posted_writes == initial_posts,
              "blocked SHR record changed or bypassed renderer backpressure");
    end
    @(negedge clk); video_record_ready = 1;
    wait_marker(0, 1, 600000);
    check(mb_ram[16'h4000] == 8'hA6 && cnt_posted_writes == 1,
          "SHR lost old classic mirror work or enqueued new mirror data");
    check(mb_aux[16'h2000] == 8'h33 && mb_aux[16'h9D00] == 8'h44 &&
          mb_aux[16'h9E00] == 8'h55 && mb_ram[16'h2000] == 8'h22 &&
          mb_ram[16'h0400] == 8'h17 && mb_ram[16'h0800] == 8'h18 &&
          mb_aux[16'h0800] == 8'h19,
          "SHR framebuffer or classic video work memory reached the motherboard");
    check(renderer_shadow[17'h12000] == 8'hA7 &&
          renderer_shadow[17'h19D00] == 8'h93 &&
          renderer_shadow[17'h19E00] == 8'h95 &&
          renderer_shadow[17'h02000] == 8'hB7 &&
          renderer_shadow[17'h00400] == 8'hB4 &&
          renderer_shadow[17'h00800] == 8'hB8 &&
          renderer_shadow[17'h04000] == 8'hB9,
          "SHR renderer shadow lost a final captured value");
    @(negedge clk); arm_rw_flush_req = 1;
    @(negedge clk); arm_rw_flush_req = 0;
    guard = 0;
    while (!arm_rw_flush_done && guard < 200000) begin
        @(posedge clk); #1ps;
        guard++;
    end
    check(guard < 200000 && arm_rw_hold_state && cnt_posted_writes == 1 &&
          !dut.video_mirror_pending, "PRIVATE hold invented an SHR mirror flush");
    sh_check(18'h12000, 8'hA7);
    sh_check(18'h19D00, 8'h93);
    sh_check(18'h02000, 8'hB7);
    sh_check(18'h00400, 8'hB4);
    sh_check(18'h00800, 8'hB8);
    sh_check(18'h0A000, 8'hBC);
    sh_write(18'h0A110, 8'h01);
    @(negedge clk); arm_rw_hold_release = 1;
    @(negedge clk); arm_rw_hold_release = 0;
    wait_marker(1, 1, 600000);
    await_video_drain();
    check(!shr_capture_active && cnt_posted_writes == 2 &&
          mb_aux[16'h2000] == 8'h7C && mb_aux[16'h9D00] == 8'h44 &&
          shr_observed_count == shr_expected_count,
          "SHR exit changed old pixels, missed capture or broke classic mirroring");
    @(negedge clk); core_run = 0;
    repeat (1000) @(posedge clk);
    check(cnt_posted_writes == 2 && !dut.video_mirror_pending,
          "cold handback generated retroactive SHR mirrors");
    shr_check_records = 0;
    $display("VTW SHR CAPTURE CASE PASS speed=%0d paging=%0d IIplus=%0d records=%0d",
             mode, paging, iiplus, shr_observed_count);
endtask

initial begin
    for (int mode = 0; mode < 4; mode++)
        shr_capture_case(2'(mode), 0, 0);
    shr_capture_case(2'd3, 1, 0);
    shr_capture_case(2'd3, 2, 0);
    shr_capture_case(2'd3, 1, 1);
    $display("VTW SHR CAPTURE PASS");
    $finish;
end
