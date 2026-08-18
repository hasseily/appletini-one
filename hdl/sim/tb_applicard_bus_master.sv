`timescale 1ns / 1ps

// Pin-level regression for the AD8088 sparse Apple-memory bus master.
//
// The applicard_card unit bench checks only its internal AppleBus_write
// request.  This bench carries that request through the real arbiter and
// apple_bus_wrapper, then verifies that a motherboard-style RAM model sees
// the write and supplies the following read.  This is the boundary that a
// falsely acknowledged bus transaction can otherwise bypass.
module tb_applicard_bus_master;
    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;       // 133.333 MHz

    logic phi0 = 1'b0;
    always #490 phi0 = ~phi0;      // ~1.02 MHz

    logic rstn = 1'b0;

    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
    wire        apple_inh_pin;
    wire        apple_res_pin;
    wire        apple_irq_pin;
    wire        apple_rdy_pin;
    wire        apple_dma_pin;
    wire        apple_nmi_pin;

    // Model the real bus residue instead of making every released cycle look
    // like an inert $FFFF read. Strong card/motherboard drivers update these
    // weakly held values; a test can also inject the last motherboard cycle at
    // the instant DMA asserts.
    logic [7:0]  held_data_q = 8'hFF;
    logic [15:0] held_addr_q = 16'hFFFF;
    logic        held_rw_q = 1'b1;
    logic [7:0]  injected_data_q = 8'hFF;
    logic [15:0] injected_addr_q = 16'hFFFF;
    logic        injected_rw_q = 1'b1;
    logic        inject_on_dma_q = 1'b0;

    // Residue decay. An undriven line holds its level briefly, then sags
    // LOW with per-bit staggered timing -- the polarity proven by hardware
    // crash dumps (staged bytes rewritten as bit-cleared copies of
    // themselves). R/W sags first: its droop is what turns a residue cycle
    // into a rogue write at the DRAM. The model runs only while /DMA owns
    // the bus; outside ownership this bench has no CPU model, so the held
    // value simply persists as before.
    localparam int DECAY_HOLD_CLKS = 27;   // ~200 ns at 133.333 MHz
    localparam int DECAY_STEP_CLKS = 11;   // ~80 ns per additional bit
    logic [15:0] addr_decay_mask_q = '1;
    logic [7:0]  data_decay_mask_q = '1;
    logic        rw_decay_mask_q   = 1'b1;
    int          addr_float_clks_q = 0;
    int          data_float_clks_q = 0;

    assign (weak0, weak1) apple_data_pin = held_data_q & data_decay_mask_q;
    assign (weak0, weak1) apple_addr_pin = held_addr_q & addr_decay_mask_q;
    assign (weak0, weak1) apple_rw_pin   = held_rw_q & rw_decay_mask_q;
    assign (weak0, weak1) apple_inh_pin  = 1'b1;
    assign (weak0, weak1) apple_res_pin  = 1'b1;
    assign (weak0, weak1) apple_irq_pin  = 1'b1;
    assign (weak0, weak1) apple_rdy_pin  = 1'b1;
    assign (weak0, weak1) apple_dma_pin  = 1'b1;
    assign (weak0, weak1) apple_nmi_pin  = 1'b1;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write card_write;
    globals::AppleBus_write ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    logic tini_oe_pin;
    logic tini_addr_dir_pin;
    logic tini_data_dir_pin;

    logic [31:0] dbg_ghost_write_w;
    apple_bus_wrapper wrapper_i (
        .clk(clk),
        .rstn(rstn),
        .physical_bus_isolate(1'b0),
        .res_filtered_out(),
        .dbg_lost_cycle_count(),
        .dbg_bus_quality(),
        .dbg_tap_mismatch(),
        .dbg_strobe_anom(),
        .dbg_tap_last(),
        .dbg_ghost_write(dbg_ghost_write_w),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1),
        .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0),
        .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read),
        .ab_write(ab_write)
    );

    apple_bus_write_arbiter #(.NUM_CLIENTS(1)) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes({card_write}),
        .ab_write(ab_write)
    );

    applicard_card card_i (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .card_enabled(1'b1),
        .disk2_timing_active(1'b0),
        .vtw_enabled(1'b0),
        .vtw_bus_owned(1'b0),
        .slot_assign(3'h5),
        .as_common(as_common),
        .as_client(axi),
        .ab_write(card_write)
    );

    // Motherboard RAM drives reads during PHI0. A IIe write is captured at
    // the CAS point, not at PH0 fall: data must appear only after the bus
    // turnaround floor and must already be valid when CAS falls.
    logic [7:0] mb_ram [0:16'hFFFF];
    wire mb_drive_data = phi0 && (apple_rw_pin === 1'b1) &&
                         !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_ram[apple_addr_pin] : 8'hzz;

    always @(posedge clk) begin
        if (apple_dma_pin !== 1'b0 || tini_addr_dir_pin) begin
            addr_float_clks_q <= 0;
            addr_decay_mask_q <= '1;
            rw_decay_mask_q   <= 1'b1;
        end else begin
            addr_float_clks_q <= addr_float_clks_q + 1;
            if (addr_float_clks_q > DECAY_HOLD_CLKS)
                rw_decay_mask_q <= 1'b0;
            for (int b = 0; b < 16; b++)
                if (addr_float_clks_q >
                    DECAY_HOLD_CLKS + DECAY_STEP_CLKS * (b + 1))
                    addr_decay_mask_q[b] <= 1'b0;
        end
        if (apple_dma_pin !== 1'b0 || tini_data_dir_pin ||
            mb_drive_data) begin
            data_float_clks_q <= 0;
            data_decay_mask_q <= '1;
        end else begin
            data_float_clks_q <= data_float_clks_q + 1;
            for (int b = 0; b < 8; b++)
                if (data_float_clks_q >
                    DECAY_HOLD_CLKS + DECAY_STEP_CLKS * (b + 1))
                    data_decay_mask_q[b] <= 1'b0;
        end
    end

    always @(posedge clk or negedge apple_dma_pin) begin
        if (apple_dma_pin === 1'b0 && inject_on_dma_q) begin
            held_addr_q      <= injected_addr_q;
            held_rw_q        <= injected_rw_q;
            held_data_q      <= injected_data_q;
            inject_on_dma_q  <= 1'b0;
        end else begin
            if (tini_addr_dir_pin) begin
                held_addr_q <= apple_addr_pin;
                held_rw_q   <= apple_rw_pin;
            end
            if (tini_data_dir_pin || mb_drive_data)
                held_data_q <= apple_data_pin;
        end
    end

    int write_edges = 0;
    int write_windows = 0;
    realtime phi0_rise_at = 0.0;
    realtime data_start_delta = 0.0;
    realtime data_end_delta = 0.0;
    realtime dma_assert_at = 0.0;
    realtime dma_hold_delta = 0.0;
    realtime dma_hold_max = 0.0;
    logic dma_data_window_open = 1'b0;
    logic dma_session_seen = 1'b0;
    int dma_sessions = 0;
    int acquire_park_cycles = 0;
    int release_park_cycles = 0;
    int cpu_release_cycles = 0;
    int cpu_release_min = 32'h7FFF_FFFF;
    logic recovery_window_active = 1'b0;

    localparam logic [2:0] TB_BUS_DMA_GRACE_1  = 3'd1;
    localparam logic [2:0] TB_BUS_DMA_GRACE_2  = 3'd2;
    localparam logic [2:0] TB_BUS_RELEASE_PARK = 3'd6;
    localparam logic [2:0] TB_BUS_RELEASE_GUARD = 3'd7;
    int grace1_park_cycles = 0;

    always @(posedge phi0) begin
        phi0_rise_at = $realtime;
        if (rstn && recovery_window_active && apple_dma_pin === 1'b1)
            cpu_release_cycles++;
        if (rstn && apple_dma_pin === 1'b0 &&
            tini_addr_dir_pin && apple_rw_pin === 1'b0 &&
            tini_data_dir_pin) begin
            $fatal(1, "DMA write drove data at PH0 rise");
        end

        fork
            begin
                #210ns;
                /* A real motherboard has no concept of an invalid cycle:
                 * any R/W low at CAS writes the DRAM, whoever (or whatever
                 * residue) put it there. A write in an owned cycle that no
                 * master drove is the rogue-write fault this bench exists
                 * to catch. */
                if (rstn && apple_dma_pin === 1'b0 &&
                    apple_rw_pin === 1'b0) begin
                    if (!tini_addr_dir_pin)
                        $fatal(1,
                            "ghost write: owned floating bus reached CAS with R/W low at $%04X data=%02X",
                            apple_addr_pin, apple_data_pin);
                    if (tini_data_dir_pin !== 1'b1)
                        $fatal(1, "DMA write missed the 210 ns CAS deadline");
                    mb_ram[apple_addr_pin] = apple_data_pin;
                    write_edges++;
                end
            end
        join_none

        // Every owned cycle must carry either a requested address or the
        // inert park read by the time its PH0 opens -- including the cut
        // cycle at /DMA assert. Only error paths may reach the high-Z
        // guard state.
        #1ns;
        if (rstn && apple_dma_pin === 1'b0 &&
            card_i.bus_state_q == TB_BUS_DMA_GRACE_1) begin
            if (!tini_addr_dir_pin || apple_addr_pin !== 16'h0200 ||
                apple_rw_pin !== 1'b1 || tini_data_dir_pin)
                $fatal(1,
                    "cut cycle not parked by its PH0: addr=%04X rw=%b dir=%b",
                    apple_addr_pin, apple_rw_pin, tini_addr_dir_pin);
            grace1_park_cycles++;
        end
        if (rstn && apple_dma_pin === 1'b0 &&
            card_i.bus_state_q == TB_BUS_DMA_GRACE_2) begin
            if (!tini_addr_dir_pin || apple_addr_pin !== 16'h0200 ||
                apple_rw_pin !== 1'b1 || tini_data_dir_pin)
                $fatal(1, "acquisition park missing: addr=%04X rw=%b dir=%b",
                       apple_addr_pin, apple_rw_pin, tini_addr_dir_pin);
            acquire_park_cycles++;
        end
        if (rstn && apple_dma_pin === 1'b0 &&
            card_i.bus_state_q == TB_BUS_RELEASE_PARK) begin
            if (!tini_addr_dir_pin || apple_addr_pin !== 16'h0200 ||
                apple_rw_pin !== 1'b1 || tini_data_dir_pin)
                $fatal(1, "release park missing: addr=%04X rw=%b dir=%b",
                       apple_addr_pin, apple_rw_pin, tini_addr_dir_pin);
            release_park_cycles++;
        end
        if (rstn && card_i.bus_state_q == TB_BUS_RELEASE_GUARD)
            $fatal(1,
                "release guard reached: no path may float a full owned cycle");
    end

    always @(posedge tini_data_dir_pin) begin
        if (rstn && apple_dma_pin === 1'b0 && apple_rw_pin === 1'b0) begin
            data_start_delta = $realtime - phi0_rise_at;
            dma_data_window_open = 1'b1;
            write_windows++;
            if (phi0 !== 1'b1)
                $fatal(1, "DMA write began outside PH0");
            if (($realtime - phi0_rise_at) < 155.0)
                $fatal(1, "DMA write began before 155 ns: %0.1f ns",
                       $realtime - phi0_rise_at);
            if (($realtime - phi0_rise_at) > 210.0)
                $fatal(1, "DMA write began after 210 ns: %0.1f ns",
                       $realtime - phi0_rise_at);
        end
    end

    always @(negedge tini_data_dir_pin) begin
        if (dma_data_window_open) begin
            data_end_delta = $realtime - phi0_rise_at;
            if (($realtime - phi0_rise_at) < 265.0)
                $fatal(1, "DMA write ended before CAS hold: %0.1f ns",
                       $realtime - phi0_rise_at);
            if (phi0 !== 1'b1)
                $fatal(1, "DMA write leaked into PH1");
            dma_data_window_open = 1'b0;
        end
    end

    // Ownership entry and exit must happen in PH1. Address/R-W are released
    // first; /DMA may rise only after the bus is already tri-stated.
    always @(negedge apple_dma_pin) begin
        if (rstn) begin
            if (recovery_window_active) begin
                if (cpu_release_cycles < 2)
                    $fatal(1,
                        "DMA recovery gap only gave the CPU %0d full cycles",
                        cpu_release_cycles);
                if (cpu_release_cycles < cpu_release_min)
                    cpu_release_min = cpu_release_cycles;
            end
            recovery_window_active = 1'b0;
            dma_assert_at = $realtime;
            dma_session_seen = 1'b1;
            dma_sessions++;
            if (phi0 !== 1'b0 || tini_addr_dir_pin)
                $fatal(1, "DMA asserted outside a released PH1 bus");
        end
    end

    always @(posedge tini_addr_dir_pin) begin
        if (rstn && apple_dma_pin === 1'b0 &&
            ($realtime - dma_assert_at) < 30.0)
            $fatal(1, "address/R-W driven less than 30 ns after DMA");
    end

    always @(posedge apple_dma_pin) begin
        if (rstn && dma_session_seen) begin
            dma_hold_delta = $realtime - dma_assert_at;
            if (dma_hold_delta > dma_hold_max)
                dma_hold_max = dma_hold_delta;
            if (dma_hold_delta > 10000.0)
                $fatal(1, "DMA held 6502 for %0.1f ns", dma_hold_delta);
            // DMA, address direction, and pin tri-state derive from the same
            // arbiter word but settle in separate zero-delay simulation steps.
            #1ps;
            if (phi0 !== 1'b0 || tini_addr_dir_pin || tini_data_dir_pin)
                $fatal(1,
                    "DMA release invalid: phi0=%b addr_dir=%b data_dir=%b state=%0d addr_q=%b dma_q=%b aw_addr=%b aw_dma=%b drive_en=%b",
                    phi0, tini_addr_dir_pin, tini_data_dir_pin,
                    card_i.bus_state_q, card_i.bus_addr_drive_q,
                    card_i.bus_dma_q, card_write.wr_addr_rw_en,
                    card_write.assert_dma, ab_read.drive_en);
            dma_session_seen = 1'b0;
            cpu_release_cycles = 0;
            recovery_window_active = 1'b1;
        end
    end

    localparam logic [7:0] REG_MODE        = 8'h04;
    localparam logic [7:0] REG_AD_STATUS   = 8'h05;
    localparam logic [7:0] REG_AD_CONTROL  = 8'h06;
    localparam logic [7:0] REG_BUS_COMMAND = 8'h0B;
    localparam logic [7:0] REG_BUS_STATUS  = 8'h0C;
    localparam logic [7:0] REG_BUS_BUFFER0 = 8'h40;

    task automatic axi_write(input logic [7:0] idx,
                             input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = idx;
        as_common.wdata  = value;
        as_common.wstrb  = 4'hF;
        axi.awvalid      = 1'b1;
        @(negedge clk);
        axi.awvalid      = 1'b0;
    endtask

    task automatic axi_read(input logic [7:0] idx,
                            output logic [31:0] value);
        @(negedge clk);
        as_common.araddr = idx;
        @(negedge clk);
        @(negedge clk);
        value = axi.rdata;
    endtask

    task automatic wait_done(output logic [31:0] status);
        int polls = 0;
        status = 32'd0;
        while (!status[9] && polls < 50000) begin
            axi_read(REG_BUS_STATUS, status);
            polls++;
        end
        if (!status[9]) $fatal(1, "AD8088 bus transaction timed out");
        if (status[10]) $fatal(1, "AD8088 bus transaction reported error");
    endtask

    logic [31:0] status;
    logic [31:0] word;
    initial begin
        as_common = '0;
        axi.awvalid = 1'b0;
        for (int i = 0; i < 65536; i++) mb_ram[i] = 8'h00;
        mb_ram[16'h2345] = 8'h19;

        repeat (8) @(posedge clk);
        rstn = 1'b1;
        // Let the pin synchronizers and RES# filter settle.
        repeat (6) @(negedge phi0);

        axi_write(REG_MODE, 32'h0000_0001);

        // A real open bus holds the preceding CPU cycle instead of snapping to
        // $FFFF. Reproduce a stale STA $C0D0 during acquisition. It must not
        // post a ghost command or change the mailbox sequence while DMA owns
        // the pins.
        begin
            automatic logic [7:0] ad_sequence_before;
            axi_read(REG_AD_STATUS, word);
            ad_sequence_before = word[31:24];
            injected_addr_q = 16'hC0D0;
            injected_rw_q = 1'b0;
            injected_data_q = 8'hFE;
            inject_on_dma_q = 1'b1;
            axi_write(REG_BUS_COMMAND, 32'h8100_2345);
            wait_done(status);
            axi_read(REG_AD_STATUS, word);
            if (word[31:24] !== ad_sequence_before || word[0])
                $fatal(1,
                    "owned bus residue posted ghost mailbox command: %08X",
                    word);
        end

        // Write $A5 to Apple memory $2345.
        axi_write(REG_BUS_COMMAND, 32'h80A5_2345);
        wait_done(status);
        if (write_edges != 1)
            $fatal(1, "expected one physical CAS write, got %0d", write_edges);
        if (write_windows != 1)
            $fatal(1, "expected one legal write window, got %0d",
                   write_windows);
        if (mb_ram[16'h2345] !== 8'hA5)
            $fatal(1, "Apple RAM write did not land: got %02X", mb_ram[16'h2345]);

        // Read it back through the same sparse bus-master path.
        axi_write(REG_BUS_COMMAND, 32'h8100_2345);
        wait_done(status);
        if (status[7:0] !== 8'hA5)
            $fatal(1, "Apple RAM readback got %02X, expected A5", status[7:0]);

        // The hardware must reject a hold longer than four bytes even if PS
        // is faulty. This keeps the 6502 register-retention limit enforced in
        // PL, not just by the service code.
        begin
            automatic int sessions_before = dma_sessions;
            axi_write(REG_BUS_COMMAND, 32'hC104_2400);
            axi_read(REG_BUS_STATUS, status);
            if (!status[9] || !status[10] ||
                dma_sessions != sessions_before)
                $fatal(1, "unsafe five-byte DMA command was accepted: %08X",
                       status);
        end

        // A four-byte read must acquire DMA once, advance one byte per Apple
        // cycle, and expose the complete safe batch through the AXI buffer.
        for (int i = 0; i < 4; i++)
            mb_ram[16'h2400 + i] = 8'h40 ^ i[7:0];
        begin
            automatic int sessions_before = dma_sessions;
            axi_write(REG_BUS_COMMAND, 32'hC103_2400);
            wait_done(status);
            if (dma_sessions != sessions_before + 1)
                $fatal(1, "bulk read used %0d DMA sessions, expected one",
                       dma_sessions - sessions_before);
            if (!status[12] || status[23:16] != 8'd3)
                $fatal(1, "bulk read status/index invalid: %08X", status);
        end
        axi_read(REG_BUS_BUFFER0, word);
        for (int byte_index = 0; byte_index < 4; byte_index++) begin
            if (word[byte_index*8 +: 8] !== (8'h40 ^ byte_index))
                $fatal(1, "bulk read mismatch at %0d: got %02X",
                       byte_index, word[byte_index*8 +: 8]);
        end

        // Preload the same staging buffer and write four consecutive bytes
        // under one DMA acquisition. The existing CAS assertions cover every
        // byte in the burst, not only its first cycle.
        word = 32'hA3A2_A1A0;
        axi_write(REG_BUS_BUFFER0, word);
        begin
            automatic int sessions_before = dma_sessions;
            automatic int writes_before = write_edges;
            axi_write(REG_BUS_COMMAND, 32'hC003_3000);
            wait_done(status);
            if (dma_sessions != sessions_before + 1)
                $fatal(1, "bulk write used %0d DMA sessions, expected one",
                       dma_sessions - sessions_before);
            if (write_edges != writes_before + 4)
                $fatal(1, "bulk write produced %0d CAS writes, expected 4",
                       write_edges - writes_before);
        end
        for (int i = 0; i < 4; i++) begin
            if (mb_ram[16'h3000 + i] !== (8'hA0 + i[7:0]))
                $fatal(1, "bulk write mismatch at %0d: got %02X",
                       i, mb_ram[16'h3000 + i]);
        end

        // Reproduce Reboot Camp's full padded IO.SYS + MSDOS.SYS transfer.
        // Each command must advance the sequence once, keep one DMA session,
        // and return every source byte without a skip or stale buffer word.
        for (int i = 0; i < 24763; i++)
            mb_ram[16'h2000 + i] = (i * 37 + 8'h5A) & 8'hFF;
        begin
            automatic int offset = 0;
            automatic int sessions_before = dma_sessions;
            while (offset < 24763) begin
                automatic int chunk = 24763 - offset;
                automatic logic [7:0] sequence_before;
                if (chunk > 4) chunk = 4;
                axi_read(REG_BUS_STATUS, word);
                sequence_before = word[31:24];
                axi_write(REG_BUS_COMMAND,
                    32'hC100_0000 | ((chunk - 1) << 16) |
                    (16'h2000 + offset));
                wait_done(status);
                if (status[31:24] !== sequence_before + 8'd1)
                    $fatal(1, "bulk sequence did not advance at %0d: %08X",
                           offset, status);
                if (!status[12] || status[23:16] !== chunk - 1)
                    $fatal(1, "bulk status/index invalid at %0d: %08X",
                           offset, status);
                axi_read(REG_BUS_BUFFER0, word);
                for (int byte_index = 0; byte_index < chunk;
                     byte_index++) begin
                    if (word[byte_index*8 +: 8] !==
                        (((offset + byte_index) * 37 + 8'h5A) & 8'hFF))
                        $fatal(1, "full copy mismatch at %0d: got %02X",
                               offset + byte_index,
                               word[byte_index*8 +: 8]);
                end
                offset += chunk;
            end
            if (dma_sessions != sessions_before + 6191)
                $fatal(1, "full copy used %0d DMA sessions, expected 6191",
                       dma_sessions - sessions_before);
        end

        // A monitor reset is also a hard DMA cancel. It must release every
        // pin and return DONE+ERROR instead of leaving PS blocked forever.
        axi_write(REG_BUS_COMMAND, 32'hC103_2400);
        wait (apple_dma_pin === 1'b0);
        repeat (2) @(posedge clk);
        axi_write(REG_AD_CONTROL, 32'h0000_0040);
        repeat (4) @(negedge phi0);
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || !status[10] || status[8])
            $fatal(1, "cancel did not report terminal error: %08X", status);
        if (apple_dma_pin !== 1'b1 || tini_addr_dir_pin || tini_data_dir_pin)
            $fatal(1, "cancel left Apple bus pins driven");
        if (grace1_park_cycles == 0 || acquire_park_cycles == 0 ||
            release_park_cycles == 0)
            $fatal(1, "park-cycle monitors never observed the bus sequence");
        if (dbg_ghost_write_w[31:16] != 16'd0)
            $fatal(1,
                "wrapper ghost-write meter counted %0d (last addr $%04X)",
                dbg_ghost_write_w[31:16], dbg_ghost_write_w[15:0]);

        $display("APPLICARD BUS MASTER PASS (sessions=%0d, max DMA hold %0.1f ns, min CPU recovery %0d cycles, write window %0.1f..%0.1f ns, ghost writes %0d)",
                 dma_sessions, dma_hold_max, cpu_release_min,
                 data_start_delta, data_end_delta,
                 dbg_ghost_write_w[31:16]);
        $finish;
    end

    initial begin
        #100ms;
        $fatal(1, "tb_applicard_bus_master timeout");
    end
endmodule
