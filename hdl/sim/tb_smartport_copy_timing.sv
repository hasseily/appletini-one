`timescale 1ns / 1ps

// Compare the stock SmartPort ROM RDPAGE with the same bytes in main RAM.
// A preloaded FIFO removes ARM dispatch and storage from the measurement.
// The private SmartPort shortcut stays off, as it does in ONE//e.
module tb_smartport_copy_timing;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic vtw_enable = 1'b0;
    logic core_run = 1'b0;

    globals::AppleBus_read ab_read;
    globals::AppleBus_read softswitch_ab_read;
    globals::AppleBus_write motherboard_write;
    globals::AppleBus_write smartport_write;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write [12:0] client_writes;
    globals::AppleBus_write merged_write;
    globals::SoftSwitchState sss;

    logic req_ready;
    logic resp_valid;
    logic [7:0] resp_rdata;

    // Use the production PAL cadence: 131 fabric clocks per native tick.
    apple_virtual_bus virtual_bus_i (
        .clk(clk),
        .resetn(resetn),
        .video_mode_50hz(1'b1),
        .res_n_in(1'b1),
        .irq_n_in(1'b1),
        .nmi_n_in(1'b1),
        .rdy_n_in(1'b1),
        .dma_n_in(1'b1),
        .inh_n_in(1'b1),
        .req_valid(1'b0),
        .req_ready(req_ready),
        .req_addr(16'hFFFF),
        .req_rw(1'b1),
        .req_wdata(8'h00),
        .resp_valid(resp_valid),
        .resp_rdata(resp_rdata),
        .floating_bus_data(8'h00),
        .ab_write(merged_write),
        .ab_read(ab_read)
    );

    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    wire video_vblank = (line_in_frame >= 9'd192);

    apple_timing_gen timing_i (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(ab_read.sss_en),
        .video_mode_50hz(1'b1),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(1'b0),
        .set_vblank_start_pulse(1'b0)
    );

    logic keyboard_event_ready;
    onee_motherboard_io motherboard_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(1'b1),
        .ab_read(ab_read),
        .sss(sss),
        .softswitch_ab_read(softswitch_ab_read),
        .ab_write(motherboard_write),
        .floating_bus_data(8'h00),
        .video_vblank(video_vblank),
        .keyboard_event_valid(1'b0),
        .keyboard_event_ready(keyboard_event_ready),
        .keyboard_event_code(7'h00),
        .keyboard_any_down(1'b0),
        .keyboard_modifiers_in(3'b000),
        .keyboard_modifiers_state(),
        .keyboard_latch(),
        .keyboard_strobe(),
        .pushbuttons(3'b000),
        .cassette_in(1'b0),
        .paddle_values(32'h8080_8080),
        .cassette_out(),
        .speaker(),
        .utility_strobe_pulse(),
        .annunciators(),
        .ioudis(),
        .paddle_active(),
        .paddle_trigger_pulse()
    );

    soft_switch_manager manager_i (
        .clk(clk),
        .rstn(resetn),
        .ramworks_en(1'b0),
        .ab_read(softswitch_ab_read),
        .sss(sss)
    );

    always_comb begin
        client_writes = '{default: '0};
        client_writes[3] = smartport_write;
        client_writes[11] = vtw_write;
        client_writes[12] = motherboard_write;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(13),
        .FAST_DATA_CLIENT(2),
        .FAST_ADDR_CLIENT(11)
    ) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes(client_writes),
        .ab_write(merged_write)
    );


    logic [1:0] mode = 2;
    logic record_enable = 1;
    globals::AxiSimple_common as_common;
    AxiSimple_if sp_axi();
    smartport_card card_i (
        .clk(clk), .rstn(resetn), .ab_read(ab_read), .apple_bus_visible(1'b1),
        .overlay_bus_visible(1'b0), .sss(sss), .slot_assign(3'd7),
        .as_common(as_common), .as_client(sp_axi), .ab_write(smartport_write),
        .smartport_irq(), .overlay_capture_drop(1'b0), .overlay_canvas_shr_active(1'b0),
        .overlay_devsel_enabled(), .overlay_capture_armed(), .overlay_capture_bank_aux(),
        .overlay_capture_base(), .overlay_capture_limit(), .vtw_valid(1'b0),
        .vtw_target(3'd0), .vtw_addr(11'd0), .vtw_rw(1'b1), .vtw_wdata(8'd0),
        .vtw_sss_snapshot(22'd0), .vtw_ready(), .vtw_resp_valid(), .vtw_resp_rdata()
    );
    logic [15:0] dbg_core_pc;
    logic bus_owned;
    logic [31:0] core_cycles;
    logic [31:0] bus_cycles;

    vtw_core_top core_i (
        .clk(clk),
        .rstn(resetn),
        .enable(vtw_enable),
        .host_is_iiplus(1'b0),
        .virtual_motherboard(1'b1),
        .core_run(core_run),
        .pause(1'b0),
        .assert_apple_res(1'b0),
        .speed_mode(mode),
        .pace_divider(16'd0),
        .ignore_c074(1'b0),
        .slow_region_en(10'd0),
        .slow_duration(16'd0),
        .d2_active(1'b0),
        .d2_motor_active(1'b0),
        .d2_req_valid(),
        .d2_req_addr(),
        .d2_req_ready(1'b0),
        .d2_resp_valid(1'b0),
        .d2_resp_rdata(8'h00),
        .d2_cycle_tick(),
        .d2_native_cycle_active(),
        .d2_time_ready(1'b1),
        .d2_write_timing_active(1'b0),
        .ramworks_en(1'b0),
        .video_vbl(video_vblank),
        .video_mode_50hz(1'b1),
        .video_line(line_in_frame),
        .video_cycle(cycle_in_line),
        .post_main_wide(1'b0),
        .video_record_enable(record_enable), .video_record_ready(1'b1),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'h0000),
        .overlay_capture_limit(16'h0000),
        .ab_read(ab_read),
        .ab_write(vtw_write),
        .irq_assert_in(merged_write.assert_irq),
        .data_drive_in(merged_write.wr_data_en),
        .data_drive_value_in(merged_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .usb_joystick_active(1'b0), .usb_joystick_buttons(3'd0),
        .usb_joystick_paddles(32'h80808080),
        .rw_req_valid(),
        .rw_req_rw(),
        .rw_req_addr(),
        .rw_req_wline(),
        .rw_req_ready(1'b1),
        .rw_resp_valid(1'b0),
        .rw_resp_rline(64'h0),
        .sp_active(1'b0),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(),
        .sp_req_target(),
        .sp_req_addr(),
        .sp_req_rw(),
        .sp_req_wdata(),
        .sp_req_ready(1'b0),
        .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'h00),
        .sp_sss_snapshot(),
        .sh_en(1'b0),
        .sh_addr(18'h00000),
        .sh_we(1'b0),
        .sh_wdata(8'h00),
        .sh_rdata(),
        .arm_req_valid(1'b0),
        .arm_req_addr(16'h0000),
        .arm_req_rw(1'b1),
        .arm_req_wdata(8'h00),
        .arm_req_busy(),
        .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(1'b0),
        .arm_post_addr(16'h0000),
        .arm_post_wdata(8'h00),
        .arm_post_ready(),
        .arm_rw_flush_req(1'b0),
        .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(),
        .arm_rw_hold_state(),
        .c074_state(),
        .bus_owned(bus_owned),
        .video_phase_1mhz(),
        .dbg_core_pc(dbg_core_pc),
        .cnt_core_cycles(core_cycles),
        .cnt_bus_cycles(bus_cycles),
        .cnt_posted_writes(),
        .post_fill(),
        .post_high_water(),
        .cnt_post_drops(),
        .cnt_invalid_routes(),
        .dbg_vsss(),
        .dbg_last_sync_addr(),
        .dbg_last_sync_data(),
        .dbg_last_sync_rw(),
        .dbg_irq_edges(),
        .dbg_cxxx_ring(),
        .dbg_c0_ring(),
        .dbg_sync_write_check(),
        .dbg_sync_write_addr(),
        .dbg_c000_context(),
        .dbg_c000_counts(),
        .dbg_pc_trace(),
        .dbg_io_trace(),
        .dbg_trace_status(),
        .dbg_bus_faults()
    );


    integer clocks = 0;
    integer started = 0, finished = 0, start_clock, end_clock;
    integer c8_reads, fifo_reads, pops, flush_clocks, posted, flush_starts;
    integer case_count = 0;
    integer elapsed [0:8];
    integer flushes [0:8];
    logic flush_prev;
    always @(posedge clk) begin
        clocks <= clocks + 1;
        if (!resetn) begin
            started <= 0; finished <= 0;
            c8_reads <= 0; fifo_reads <= 0; pops <= 0;
            flush_clocks <= 0; posted <= 0; flush_starts <= 0; flush_prev <= 0;
        end else begin
            if (core_i.core_en && !core_i.core_rwb && core_i.core_addr == 16'h1FF0) begin
                started <= 1; start_clock <= clocks;
            end
            if (core_i.core_en && !core_i.core_rwb && core_i.core_addr == 16'h1FF1) begin
                finished <= 1; end_clock <= clocks;
            end
            if (started && !finished) begin
                if (ab_read.serve_en && ab_read.rw) begin
                    if (ab_read.addr >= 16'hC800 && ab_read.addr < 16'hCFF0) c8_reads <= c8_reads + 1;
                    if (ab_read.addr == 16'hCFF0) fifo_reads <= fifo_reads + 1;
                end
                if (ab_read.data_en && !ab_read.rw && ab_read.addr == 16'hCFF2) pops <= pops + 1;
                if (core_i.video_sync_active) flush_clocks <= flush_clocks + 1;
                if (core_i.video_sync_active && !flush_prev) flush_starts <= flush_starts + 1;
                if (core_i.eng_post_we) posted <= posted + 1;
            end
            flush_prev <= core_i.video_sync_active;
        end
    end
    task automatic byte_at(input integer addr, input logic [7:0] value);
        core_i.shadow_i.mem_main[addr>>2][8*(addr&3)+:8] = value;
    endtask
    integer p;
    task automatic emit(input logic [7:0] value);
        byte_at(p,value);p++;
    endtask
    task automatic word_at(input integer value);
        emit(value[7:0]);emit(value[15:8]);
    endtask
    task automatic axi_write(input logic [7:0] addr,input logic [31:0] value);
        @(negedge clk); as_common.awaddr=addr;as_common.wdata=value;as_common.wstrb=15;sp_axi.awvalid=1;
        @(negedge clk); sp_axi.awvalid=0;repeat(2) @(negedge clk);
    endtask
    task automatic run_case(input logic [1:0] speed,input logic rom,input logic rec,input integer dest);
        integer entry;
        @(negedge clk);resetn=0;vtw_enable=0;core_run=0; mode=speed; record_enable=rec;
        repeat(8) @(negedge clk);
        for(integer i=0;i<16384;i++) begin
            core_i.shadow_i.mem_main[i]=0;core_i.shadow_i.mem_aux[i]=0;
        end
        for(integer i=0;i<4096;i++) core_i.shadow_i.mem_rom[i]=32'hEAEAEAEA;
        core_i.shadow_i.mem_rom[4095]=32'h20002000;
        // Byte-for-byte copy of production RDPAGE, relocated only for the RAM case.
        p=16'h2100;
        emit(8'hAD);word_at(16'hCFF0); emit(8'h8D);word_at(16'hCFF2);
        emit(8'h91);emit(8'h44);emit(8'hC8);emit(8'hD0);emit(8'hF5);emit(8'h60);
        entry=rom ? 16'hC9C5 : 16'h2100;
        p=16'h2000;emit(8'h78);emit(8'hD8);
        emit(8'hA9);emit(0);emit(8'h8D);word_at(16'hC006);
        emit(8'h2C);word_at(16'hCFFF);emit(8'h2C);word_at(16'hC700);
        emit(8'hA9);emit(dest[7:0]);emit(8'h85);emit(8'h44);
        emit(8'hA9);emit(dest[15:8]);emit(8'h85);emit(8'h45);
        emit(8'hA0);emit(0);emit(8'hA9);emit(1);
        emit(8'h8D);word_at(16'h1FF0);
        emit(8'h20);word_at(entry);emit(8'hE6);emit(8'h45);
        emit(8'h20);word_at(entry);
        // Drain the last pending video byte before ending either copy path.
        emit(8'h2C);word_at(16'hCFF1);
        emit(8'h8D);word_at(16'h1FF1);emit(8'h4C);word_at(p-1);
        resetn=1;repeat(140000) @(negedge clk);
        axi_write(3,6);
        for(integer i=0;i<128;i++) axi_write(5,{8'(i*4+3),8'(i*4+2),8'(i*4+1),8'(i*4)});
        axi_write(3,8);
        vtw_enable=1;wait(bus_owned);repeat(8) @(negedge clk);core_run=1;
        fork : done_wait
            begin wait(finished); disable done_wait;end
            begin #50ms;$fatal(1,"copy timeout PC=%04X fifo=%0d mode=%0d rom=%0d",dbg_core_pc,card_i.out_count_q,mode,rom);end
        join
        for(integer i=0;i<512;i++) begin
            if(core_i.shadow_i.mem_main[(dest+i)>>2][8*((dest+i)&3)+:8]!==8'(i))
                $fatal(1,"copy mismatch i=%0d",i);
        end
        if (card_i.out_count_q != 0 || pops != 512) $fatal(1,"FIFO consumption mismatch %0d %0d",card_i.out_count_q,pops);
        elapsed[case_count] = end_clock-start_clock;
        flushes[case_count] = flush_starts;
        case_count++;
        $display("COPY RESULT mode=%0d rom=%0d record=%0d dest=%04X clocks=%0d ticks=%0f c8=%0d fifo=%0d pop=%0d flush_clocks=%0d flush_starts=%0d posted=%0d",
            mode,rom,rec,dest,end_clock-start_clock,real'(end_clock-start_clock)/131.0,c8_reads,fifo_reads,pops,flush_clocks,flush_starts,posted);
    endtask
    initial begin
        as_common='0;sp_axi.awvalid=0;
        run_case(2,1,1,16'h4751);
        run_case(3,1,1,16'h4751);
        run_case(3,0,1,16'h4751);
        run_case(2,0,1,16'h4751);
        run_case(3,1,0,16'h4751);
        run_case(3,0,0,16'h4751);
        run_case(3,1,1,16'h9000);
        run_case(3,0,1,16'h9000);
        run_case(2,1,1,16'h9000);
        if (elapsed[1] <= elapsed[0] || elapsed[6] >= elapsed[8])
            $fatal(1,"expected ROM speed reversal only for the HGR buffer");
        if (flushes[1] != 512 || flushes[6] != 0 || flushes[4] != 0)
            $fatal(1,"expected one mirror flush per HGR byte and none in controls");
        if (elapsed[0] != elapsed[8] || elapsed[1] <= elapsed[4])
            $fatal(1,"buffer/mirror controls did not isolate the TURBO cost");
        $display("SMARTPORT COPY TIMING PASS");$finish;
    end
    initial begin #500ms;$fatal(1,"global timeout");end
endmodule
