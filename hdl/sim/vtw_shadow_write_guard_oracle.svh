// The focused test appends this to an out-of-tree copy of the current core.
// The reference is the original retirement expression, not its local rewrite.
    integer write_guard_cycles = 0;
    integer write_guard_writes = 0;
    integer write_guard_live_drop = 0;
    logic [31:0] write_guard_states = 0;
    logic [3:0] write_guard_speeds = 0;
    always @(posedge clk) begin
        if (rstn) begin
            if ($isunknown(xstate_q))
                $fatal(1, "SHADOW WRITE GUARD FSM UNKNOWN in %m");
            if (turbo_shadow_write !==
                ((xstate_q == X_TURBO_DONE) && core_en && !cycle_rw_q))
                $fatal(1, "SHADOW WRITE GUARD EQUATION MISMATCH in %m state=%0d",
                       xstate_q);
            write_guard_cycles = write_guard_cycles + 1;
            write_guard_states[xstate_q] = 1'b1;
            write_guard_speeds[speed_mode] = 1'b1;
            if (turbo_shadow_write)
                write_guard_writes = write_guard_writes + 1;
            if (core_res_n && !core_active)
                write_guard_live_drop = write_guard_live_drop + 1;
        end
    end
    final begin
        $display("SHADOW WRITE GUARD COVERAGE %m cycles=%0d writes=%0d states=%08h speeds=%01h live_drop=%0d",
                 write_guard_cycles, write_guard_writes, write_guard_states,
                 write_guard_speeds, write_guard_live_drop);
    end
