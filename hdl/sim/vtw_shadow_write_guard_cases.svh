// Included only by tb_vtw_turbo with VTW_SHADOW_WRITE_GUARDS defined.
// All stimuli use public ports and real CPU instructions; no forced FSM states.

    integer target_writes = 0;
    integer guard_cases = 0;
    always @(posedge clk) begin
        if (!rstn) target_writes <= 0;
        else if (dut.shadow_a_en && dut.shadow_a_we &&
                 dut.shadow_a_addr == 18'h09000)
            target_writes <= target_writes + 1;
    end

    task automatic parked_write_case(input integer kind);
        integer timeout;
        integer count_before;
        integer retired_before;
        begin_program(2'd3);
        disk2_time_ready = 1'b1;
        sh_write(18'h09000, 8'h00);
        sh_write(18'h09001, 8'hCC);
        immediate(8'hA9, 8'h11);
        absolute(8'h8D, 16'h9000); // Populate the write translation.
        immediate(8'hA9, 8'h42);
        absolute(8'h8D, 16'h9000); // This real CPU write must hit the cache.
        absolute(8'h8D, 16'hA100);
        halt_loop();
        start_program();
        timeout = 0;
        while (!pause && timeout < 200000) begin
            @(negedge clk);
            if (dut.xstate_q == dut.X_TURBO_DONE && dut.turbo_hit &&
                !dut.cycle_rw_q && dut.cycle_addr_q == 16'h9000 &&
                dut.cycle_wdata_q == 8'h42)
                pause = 1'b1;
            timeout++;
        end
        check(pause, "did not park the second real cached write");
        #1ps;
        count_before = target_writes;
        check(count_before == 1, "parked store already wrote or warm store missing");
        repeat (12) @(posedge clk);
        #1ps;
        check(target_writes == count_before, "pause duplicated cached write");
        sh_check(18'h09000, 8'h11);
        @(negedge clk);
        case (kind)
            0: begin // Plain resume.
                pause = 1'b0;
            end
            1: begin // Real Disk II retirement barrier, no cache invalidation.
                disk2_time_ready = 1'b0;
                pause = 1'b0;
                repeat (12) @(posedge clk);
                #1ps;
                check(target_writes == count_before && !dut.turbo_shadow_write,
                      "Disk II barrier did not suppress cached store");
                check(dut.xstate_q == dut.X_TURBO_DONE,
                      "Disk II barrier did not retain cached store");
                @(negedge clk);
                disk2_time_ready = 1'b1;
            end
            2: begin // Flush invalidates parked tuple; hold blocks retirement.
                arm_rw_flush_req = 1'b1;
                pause = 1'b0;
                #1ps;
                check(!dut.turbo_shadow_write, "flush request admitted cached write");
                @(negedge clk);
                arm_rw_flush_req = 1'b0;
                timeout = 0;
                while (!arm_rw_flush_done && timeout < 200000) begin
                    @(posedge clk);
                    #1ps;
                    timeout++;
                end
                check(arm_rw_hold_state, "flush did not enter ownership hold");
                retired_before = cnt_core_cycles;
                sh_write(18'h09001, 8'hA7); // Adjacent byte, never same-port collision.
                repeat (16) @(posedge clk);
                #1ps;
                check(cnt_core_cycles == retired_before && !dut.turbo_shadow_write,
                      "held CPU retired or committed a TURBO store");
                @(negedge clk);
                arm_rw_hold_release = 1'b1;
                @(negedge clk);
                arm_rw_hold_release = 1'b0;
            end
            3: begin // External lane update invalidates a live paused hit.
                sh_write(18'h09001, 8'hA7);
                pause = 1'b0;
            end
            4: begin
                overlay_capture_armed = 1'b1;
                pause = 1'b0;
            end
            5: begin
                post_main_wide = 1'b1;
                pause = 1'b0;
            end
            6: begin
                ramworks_en = 1'b1;
                pause = 1'b0;
            end
            7: begin
                speed_mode = 2'd0;
                pause = 1'b0;
            end
            8: begin
                speed_mode = 2'd1;
                pace_divider = 16'd23;
                pause = 1'b0;
            end
            9: begin
                speed_mode = 2'd2;
                pause = 1'b0;
            end
            10: begin // Global reset at parked hit; no extra completing edge.
                rstn = 1'b0;
                enable = 1'b0;
                core_run = 1'b0;
                pause = 1'b0;
                repeat (12) @(posedge clk);
                @(negedge clk);
                rstn = 1'b1;
                repeat (12) @(posedge clk);
                sh_check(18'h09000, 8'h11);
            end
        endcase
        if (kind != 10) begin
            wait_marker(0, 1);
            freeze_core();
            check(target_writes == count_before + 1,
                  $sformatf("lost/duplicate reissued store kind=%0d count=%0d", kind, target_writes));
            sh_check(18'h09000, 8'h42);
            sh_check(18'h09001, (kind == 2 || kind == 3) ? 8'hA7 : 8'hCC);
        end
        guard_cases++;
        $display("SHADOW WRITE GUARD CASE PASS kind=%0d", kind);
    endtask

    task automatic status_completion_case(input logic [1:0] mode);
        begin_program(mode);
        pace_divider = 16'd23;
        absolute(8'hAD, 16'hC011);
        absolute(8'h8D, 16'hA100);
        absolute(8'hAD, 16'hC019);
        absolute(8'h8D, 16'hA101);
        halt_loop();
        start_program();
        wait_marker(1, 1);
        freeze_core();
        check(dut.write_guard_states[dut.X_STATUS_DONE],
              "status completion path was not observed");
        $display("SHADOW WRITE GUARD STATUS PASS speed=%0d", mode);
    endtask

    initial begin
        for (int kind = 0; kind <= 10; kind++) parked_write_case(kind);
        // These original fixture cases abort between live enable/RESET loss
        // and registered core_res_n, then read the real shadow byte back.
        pending_write_abort(0);
        pending_write_abort(1);
        pending_write_abort(2);
        // CPU-driven RAMRD/RAMWRT/ALTZP changes and byte coherency, not forced FSMs.
        bank_program();
        ramworks_program();
        for (int mode = 0; mode < 4; mode++) status_completion_case(2'(mode));
        deferred_display_flip(0);
        $display("SHADOW WRITE GUARDS PASS cases=%0d abort_cases=3 mapping_and_ramworks=2 status_pace_cases=4 video_barrier_cases=1", guard_cases);
        $finish;
    end
