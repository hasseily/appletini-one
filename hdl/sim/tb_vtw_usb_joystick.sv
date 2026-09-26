`timescale 1ns / 1ps
// Execute real 65C02 programs against the physical bus wrapper and USB input.
module tb_vtw_usb_joystick;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;
    logic pause = 0;
    logic host_is_iiplus = 0;
    logic virtual_motherboard = 0;
    logic iiplus_buttons_zero = 0;
    logic usb_joystick_active = 0;
    logic [2:0] usb_joystick_buttons = 0;
    logic [31:0] usb_joystick_paddles = 32'h80808080;
    logic [1:0] speed_mode = 0;
    logic [7:0] physical_status = 8'b00001001;
    wire mb_drive_data;

    // ---- Apple bus pins with motherboard-style weak pulls ----
    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
    logic       phi0 = 0;
    wire        apple_inh_pin;
    wire        apple_res_pin;
    wire        apple_irq_pin;
    wire        apple_rdy_pin;
    wire        apple_dma_pin;
    wire        apple_nmi_pin;

    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin   = 1'b1;
    assign (weak0, weak1) apple_inh_pin  = 1'b1;
    assign (weak0, weak1) apple_res_pin  = 1'b1;
    assign (weak0, weak1) apple_irq_pin  = 1'b1;
    assign (weak0, weak1) apple_rdy_pin  = 1'b1;
    assign (weak0, weak1) apple_dma_pin  = 1'b1;
    assign (weak0, weak1) apple_nmi_pin  = 1'b1;

    always #490 phi0 = ~phi0;   // ~1.02 MHz

    logic res_drive_low = 1;
    assign apple_res_pin = res_drive_low ? 1'b0 : 1'bz;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write vtw_ab_write;
    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn), .physical_bus_isolate(1'b0),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .inh_allowed(1'b1), .physical_slave_select_ok(1'b1),
        .physical_irq_allowed(1'b1), .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_devsel_n_pin(1'b1),
        .apple_inh_pin(apple_inh_pin), .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin), .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin), .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin), .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read), .ab_write(ab_write)
    );

    apple_bus_write_arbiter #(.NUM_CLIENTS(1)) arbiter_i (
        .inh_allowed(1'b1), .client_writes({vtw_ab_write}), .ab_write(ab_write)
    );

    logic        enable = 0;
    logic        core_run = 0;
    logic [9:0]  sd_region_en = 10'd0;
    logic [15:0] sd_duration  = 16'd0;
    logic        disk2_write_timing_active = 0;
    logic        sh_en = 0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;
    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_bus_cycles;
    logic        video_phase_1mhz;
    logic        disk2_active = 1'b0;
    logic        disk2_req_valid;
    logic [3:0]  disk2_req_addr;
    logic        disk2_resp_valid = 1'b0;
    logic [31:0] disk2_req_count = 32'd0;
    logic        disk2_cycle_tick;
    logic        disk2_native_cycle_active;
    logic        disk2_time_ready = 1'b1;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            disk2_resp_valid <= 1'b0;
            disk2_req_count <= 32'd0;
        end else begin
            disk2_resp_valid <= disk2_req_valid;
            if (disk2_req_valid)
                disk2_req_count <= disk2_req_count + 32'd1;
        end
    end

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(host_is_iiplus), .virtual_motherboard(virtual_motherboard),
        .core_run(core_run),
        .pause(pause),
        .assert_apple_res(1'b0),
        .speed_mode(speed_mode),
        .pace_divider(16'd37),
        .ignore_c074(1'b0),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en || virtual_motherboard),
        .data_drive_value_in(virtual_motherboard ? ab_read.data : vtw_ab_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(iiplus_buttons_zero),
        .usb_joystick_active(usb_joystick_active), .usb_joystick_buttons(usb_joystick_buttons),
        .usb_joystick_paddles(usb_joystick_paddles),
        .slow_region_en(sd_region_en),
        .slow_duration(sd_duration),
        .d2_active(disk2_active),
        .d2_req_valid(disk2_req_valid), .d2_req_addr(disk2_req_addr),
        .d2_req_ready(1'b1),
        .d2_resp_valid(disk2_resp_valid), .d2_resp_rdata(8'hA5),
        .d2_cycle_tick(disk2_cycle_tick),
        .d2_native_cycle_active(disk2_native_cycle_active),
        .d2_time_ready(disk2_time_ready),
        .d2_write_timing_active(disk2_write_timing_active),
        .ramworks_en(1'b1),
        .video_vbl(1'b0),
        .post_main_wide(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0),
        .video_mode_50hz(1'b0),
        .video_line(9'd0),
        .video_cycle(7'd0),
        .ab_read(ab_read), .ab_write(vtw_ab_write),
        .rw_req_valid(), .rw_req_rw(), .rw_req_addr(), .rw_req_wline(),
        .rw_req_ready(1'b1), .rw_resp_valid(1'b0), .rw_resp_rline(64'd0),
        .sp_active(1'b0),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(), .sp_req_target(), .sp_req_addr(), .sp_req_rw(),
        .sp_req_wdata(), .sp_req_ready(1'b1), .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'd0), .sp_sss_snapshot(),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata),
        .arm_req_valid(1'b0), .arm_req_addr('0), .arm_req_rw(1'b1),
        .arm_req_wdata('0), .arm_req_busy(), .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(1'b0), .arm_post_addr('0), .arm_post_wdata('0),
        .arm_post_ready(),
        .arm_rw_flush_req(1'b0), .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(), .arm_rw_hold_state(),
        .c074_state(), .bus_owned(),
        .video_phase_1mhz(video_phase_1mhz),
        .dbg_core_pc(), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(cnt_bus_cycles), .cnt_posted_writes(),
        .post_fill(), .post_high_water(), .cnt_post_drops(),
        .cnt_invalid_routes(),
        .dbg_vsss(), .dbg_last_sync_addr(), .dbg_last_sync_data(),
        .dbg_last_sync_rw(), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring()
    );

    logic [7:0] mb_rdata;
    always_comb begin
        mb_rdata = 8'h35;
        if (apple_addr_pin[15:4] == 12'hC06)
            mb_rdata = {physical_status[apple_addr_pin[2:0]], 7'h35};
    end
    assign mb_drive_data = phi0 && apple_rw_pin && !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_rdata : 8'hzz;

    task automatic check(input bit condition, input string message);
        if (!condition) $fatal(1, "VTW USB JOYSTICK FAIL: %s", message);
    endtask

    integer code_pos;
    integer marker_count[0:31];
    logic [7:0] marker_value[0:31];
    integer physical_reads[0:255], physical_writes[0:255];
    integer native_ticks = 0, triggers = 0;
    integer expire_tick[0:3];
    integer timer_checks = 0;
    integer last_native_completion = -1;
    always @(posedge clk) begin
        if (!rstn) begin
            native_ticks = 0;
            triggers = 0;
            last_native_completion = -1;
            for (int i = 0; i < 32; i++) begin
                marker_count[i] = 0;
                marker_value[i] = 0;
            end
            for (int i = 0; i < 256; i++) begin
                physical_reads[i] = 0;
                physical_writes[i] = 0;
            end
            for (int i = 0; i < 4; i++) expire_tick[i] = 0;
        end else begin
            if (ab_read.data_en && ab_read.cycle_valid) native_ticks++;
            if (!dut.usb_joystick_enabled)
                for (int i = 0; i < 4; i++) expire_tick[i] = native_ticks;
            else if (dut.usb_paddle_trigger) begin
                triggers++;
                for (int i = 0; i < 4; i++)
                    expire_tick[i] = native_ticks + 4 + 11 * int'(usb_joystick_paddles[8*i +: 8]);
            end
            if (ab_read.data_en && ab_read.cycle_valid && ab_read.addr[15:8] == 8'hC0) begin
                if (ab_read.rw) physical_reads[ab_read.addr[7:0]]++;
                else physical_writes[ab_read.addr[7:0]]++;
            end
            if (dut.core_en) begin
                if (dut.usb_joystick_enabled && dut.usb_paddle_poll_q) begin
                    check(dut.eff_mode == 2'd2 && native_ticks > last_native_completion,
                          "USB measurement completed more than one CPU cycle per native tick");
                    last_native_completion = native_ticks;
                end
                if (!dut.core_rwb && dut.core_addr[15:5] == (16'hA100 >> 5)) begin
                    marker_count[dut.core_addr[4:0]]++;
                    marker_value[dut.core_addr[4:0]] = dut.core_data_out;
                end
            end
            #1ps;
            for (int i = 0; i < 4; i++) begin
                check(dut.usb_paddle_active_q[i] === (native_ticks < expire_tick[i]),
                      $sformatf("paddle %0d expired at the wrong native tick", i));
                timer_checks++;
            end
        end
    end

    task automatic sh_write(input logic [17:0] addr, input logic [7:0] data);
        @(negedge clk); sh_en = 1; sh_we = 1; sh_addr = addr; sh_wdata = data;
        @(negedge clk); sh_en = 0; sh_we = 0;
    endtask
    task automatic emit(input logic [7:0] value);
        sh_write(18'h23000 + 18'(code_pos), value);
        code_pos++;
    endtask
    task automatic absolute(input logic [7:0] opcode, input logic [15:0] address);
        emit(opcode); emit(address[7:0]); emit(address[15:8]);
    endtask
    task automatic immediate(input logic [7:0] opcode, input logic [7:0] value);
        emit(opcode); emit(value);
    endtask
    task automatic save_read(input logic [15:0] address, input integer marker);
        absolute(8'hAD, address);
        absolute(8'h8D, 16'hA100 + 16'(marker));
    endtask
    task automatic halt_loop;
        absolute(8'h4C, 16'hF000 + 16'(code_pos));
    endtask
    task automatic begin_program(input logic [1:0] mode);
        @(negedge clk);
        rstn = 0; enable = 0; core_run = 0; res_drive_low = 1;
        pause = 0; host_is_iiplus = 0; virtual_motherboard = 0;
        iiplus_buttons_zero = 0; usb_joystick_active = 1;
        usb_joystick_buttons = 3'b010;
        usb_joystick_paddles = 32'h80808080;
        physical_status = 8'b00001001;
        speed_mode = mode; sd_region_en = 0; sd_duration = 0;
        repeat (20) @(posedge clk);
        @(negedge clk); rstn = 1;
        code_pos = 0;
        sh_write(18'h23FFC, 8'h00); sh_write(18'h23FFD, 8'hF0);
    endtask
    task automatic start_program;
        enable = 1; #10us;
        @(negedge clk); res_drive_low = 0; #4us;
        @(negedge clk); core_run = 1;
    endtask
    task automatic wait_marker(input integer index);
        integer guard;
        guard = 0;
        while (marker_count[index] == 0 && guard < 500000) begin
            @(posedge clk); #1ps; guard++;
        end
        check(marker_count[index] == 1, $sformatf("marker %0d missing or repeated", index));
    endtask

    task automatic status_case(input integer kind);
        logic [7:0] expected;
        begin_program(2'd3);
        host_is_iiplus = kind == 1 || kind == 3 || kind == 5;
        iiplus_buttons_zero = kind == 1 || kind == 3;
        usb_joystick_active = kind != 2 && kind != 3;
        virtual_motherboard = kind == 4;
        if (kind == 4)
            // ONE//e merges its status bit with scanner low bits from shadow.
            for (int i = 16'h0400; i < 16'h0C00; i++) sh_write(18'(i), 8'h35);
        for (int i = 0; i < 16; i++) save_read(16'hC060 + 16'(i), i);
        immediate(8'hA9, 8'hA5);
        for (int i = 0; i < 16; i++) absolute(8'h8D, 16'hC060 + 16'(i));
        absolute(8'h8D, 16'hA11F);
        halt_loop(); start_program(); wait_marker(31);
        for (int i = 0; i < 16; i++) begin
            expected = {physical_status[i % 8], 7'h35};
            if (kind == 1 && (i % 8) != 0)
                expected = {((i % 8) == 2), 7'd0};
            else if ((kind == 0 || kind == 5) && (i % 8) >= 4)
                expected = 0;
            else if ((kind == 0 || kind == 5) && (i % 8) == 2)
                expected = 8'hB5;
            // The pre-existing II+ forced-zero shortcut covers C061-3.
            else if (kind == 3 && i >= 1 && i <= 3)
                expected = 0;
            check(marker_value[i] === expected,
                  $sformatf("status kind=%0d C0%02X got=%02X expected=%02X",
                            kind, 8'h60+i, marker_value[i], expected));
            check(physical_writes[8'h60+i] == 1, "C06x write was swallowed");
            check(physical_reads[8'h60+i] ==
                  ((kind == 1 && i % 8 != 0) ||
                   ((kind == 0 || kind == 5) && i % 8 >= 4) ||
                   (kind == 3 && i >= 1 && i <= 3) ? 0 : 1),
                  "status read chose the wrong physical/private path");
        end
        $display("VTW USB STATUS PASS: kind=%0d", kind);
    endtask

    task automatic measurement_case(input logic [1:0] mode, output integer count);
        begin_program(mode);
        save_read(16'hC070, 0);
        immediate(8'hA0, 0);          // LDY #0
        absolute(8'hAD, 16'hC064);    // LDA PDL0
        immediate(8'h10, 3);         // BPL done
        emit(8'hC8);                 // INY
        immediate(8'hD0, 8'hF8);     // BNE LDA (11-cycle loop)
        absolute(8'h8C, 16'hA101);   // STY result
        halt_loop(); start_program(); wait_marker(1);
        count = marker_value[1];
        check(count >= 124 && count <= 129 && triggers == 1 &&
              physical_reads[8'h70] == 0 && physical_reads[8'h64] == 0,
              $sformatf("paddle loop lost calibration at speed %0d: %0d", mode, count));
        repeat (300) @(posedge clk);
        #1ps;
        check(!dut.usb_paddle_poll_q && dut.eff_mode == mode,
              "expired USB measurement left the CPU throttled");
        $display("VTW USB MEASUREMENT PASS: speed=%0d result=%0d", mode, count);
    endtask

    task automatic alias_case;
        begin_program(2'd3);
        // All aliases retain their physical read/write. Use zero so C074
        // keeps the selected TURBO mode and RamWorks stays on base aux.
        for (int i = 1; i < 16; i++) begin
            absolute(8'hAD, 16'hC070 + 16'(i));
            immediate(8'hA9, 0);
            absolute(8'h8D, 16'hC070 + 16'(i));
        end
        absolute(8'h8D, 16'hA100);
        immediate(8'hA9, 5); absolute(8'h8D, 16'hC071);
        absolute(8'h8D, 16'hA101);
        immediate(8'hA9, 7); absolute(8'h8D, 16'hC073);
        absolute(8'h8D, 16'hA102);
        immediate(8'hA9, 1); absolute(8'h8D, 16'hC074);
        absolute(8'h8D, 16'hA103);
        immediate(8'hA9, 0); absolute(8'h8D, 16'hC074);
        absolute(8'h8D, 16'hA104);
        halt_loop(); start_program(); wait_marker(0);
        check(triggers == 30 && dut.usb_paddle_active_q == 4'hF &&
              !dut.usb_paddle_poll_q && dut.eff_mode == 2'd3,
              "incidental trigger aliases started USB slowdown or lost a trigger");
        for (int i = 1; i < 16; i++)
            check(physical_reads[8'h70+i] == 1 && physical_writes[8'h70+i] == 1,
                  "C07x alias lost its physical transaction");
        check(dut.c074_q == 0 && dut.vsss.sw_ramworks_bank == 0,
              "C074 or RamWorks alias side effect changed");
        wait_marker(1);
        check(dut.vsss.sw_ramworks_bank == 5, "C071 lost its RamWorks bank write");
        wait_marker(2);
        check(dut.vsss.sw_ramworks_bank == 7, "C073 lost its RamWorks bank write");
        wait_marker(3);
        check(dut.c074_q == 1, "C074 lost its native-speed write");
        wait_marker(4);
        check(dut.c074_q == 0 && triggers == 34 && !dut.usb_paddle_poll_q,
              "C074 restore or alias-only timing changed");
        $display("VTW USB TRIGGER ALIASES PASS");
    endtask

    task automatic paddle_alias_case;
        begin_program(2'd3);
        usb_joystick_paddles = 32'h00FF00FF;
        absolute(8'hAD, 16'hC07F); // Alias loads timers without starting pacing.
        for (int i = 0; i < 4; i++) begin
            save_read(16'hC064 + 16'(i), i);
            save_read(16'hC06C + 16'(i), i + 4);
        end
        halt_loop(); start_program(); wait_marker(7);
        for (int i = 0; i < 4; i++)
            check(marker_value[i] === (i[0] ? 8'h00 : 8'h80) &&
                  marker_value[i+4] === marker_value[i],
                  "paddle read/alias selected the wrong timer");
        check(triggers == 1 && physical_reads[8'h7F] == 1 &&
              dut.usb_paddle_poll_q && dut.eff_mode == 2'd2,
              "paddle poll did not start native timing after an alias trigger");
        $display("VTW USB PADDLE ALIASES PASS");
    endtask

    task automatic pause_and_snapshot_case;
        integer stopped, target_tick;
        begin_program(2'd0);
        usb_joystick_paddles = 32'hFF800100;
        absolute(8'h8D, 16'hC070);    // Writes trigger too.
        halt_loop(); start_program();
        wait (triggers == 1);
        @(negedge clk); pause = 1;
        stopped = cnt_core_cycles;
        target_tick = native_ticks + 2810;
        usb_joystick_paddles = 0;     // Existing deadlines must not change.
        speed_mode = 1;
        while (native_ticks < target_tick) begin
            @(posedge clk); #1ps;
            check(cnt_core_cycles == stopped, "paused CPU advanced during paddle timing");
        end
        check(dut.usb_paddle_active_q == 0 && !dut.usb_paddle_poll_q &&
              dut.eff_mode == 1 && physical_writes[8'h70] == 0,
              "pause, mode change or new axes changed a latched paddle measurement");
        $display("VTW USB PAUSE/SNAPSHOT PASS");
    endtask

    task automatic disconnect_pending_case(input bit private_paddle);
        integer guard;
        begin_program(2'd2);
        usb_joystick_buttons = 3'b001;
        physical_status = 0;
        if (private_paddle) absolute(8'hAD, 16'hC070);
        save_read(private_paddle ? 16'hC064 : 16'hC061, 0);
        save_read(private_paddle ? 16'hC064 : 16'hC061, 1);
        halt_loop(); start_program();
        guard = 0;
        while (!(dut.cycle_addr_q == (private_paddle ? 16'hC064 : 16'hC061) &&
                 dut.xstate_q == (private_paddle ? dut.X_DEAD : dut.X_BUS)) &&
               guard < 200000) begin
            @(negedge clk); guard++;
        end
        check(guard < 200000, "disconnect did not reach the pending read");
        usb_joystick_active = 0;
        wait_marker(1);
        check(marker_value[0] === (private_paddle ? 8'h80 : 8'h35) &&
              marker_value[1] === 8'h35 && dut.usb_paddle_active_q == 0 &&
              !dut.usb_paddle_poll_q,
              "disconnect lost a captured response or failed to restore physical input");
        $display("VTW USB PENDING DISCONNECT PASS: private=%0d", private_paddle);
    endtask

    task automatic clear_case(input integer kind);
        begin_program(2'd3);
        absolute(8'hAD, 16'hC070); halt_loop(); start_program();
        wait (triggers == 1);
        @(negedge clk);
        case (kind)
            0: usb_joystick_active = 0;
            1: enable = 0;
            2: core_run = 0;
            3: res_drive_low = 1;
            4: virtual_motherboard = 1;
        endcase
        if (kind == 3) wait (!ab_read.res);
        repeat (3) @(posedge clk); #1ps;
        check(dut.usb_paddle_active_q == 0 && !dut.usb_paddle_poll_q &&
              !dut.cycle_usb_native_q,
              "USB ownership/reset boundary retained stale timer or pacing state");
        $display("VTW USB CLEAR PASS: kind=%0d", kind);
    endtask

    integer measured[0:3];
    initial begin
        for (int i = 0; i < 6; i++) status_case(i);
        for (int i = 0; i < 4; i++) measurement_case(2'(i), measured[i]);
        for (int i = 1; i < 4; i++)
            check(measured[i] >= measured[0]-1 && measured[i] <= measured[0]+1,
                  "paddle software count depends on selected CPU speed");
        alias_case();
        paddle_alias_case();
        pause_and_snapshot_case();
        disconnect_pending_case(0);
        disconnect_pending_case(1);
        for (int i = 0; i < 5; i++) clear_case(i);
        $display("VTW USB JOYSTICK PASS (%0d native timer checks)", timer_checks);
        $finish;
    end
    initial begin
        #30ms;
        $fatal(1, "VTW USB JOYSTICK FAIL: global timeout");
    end
endmodule
