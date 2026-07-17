`timescale 1ns / 1ps

// Boot-menu slot front end.
//
// The slot ROM advertises as a Disk II-style boot interface in slot 7, opens
// a reset-time menu window, forwards menu keys, removes itself, and hands off
// to SmartPort in slot 7 or Disk II in slot 6.

module boot_menu_card (
    input  logic                    clk,
    input  logic                    rstn,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    input  logic                    disk2_enabled,
    input  logic                    apple_video_mode_valid,
    input  logic                    apple_video_mode_50hz,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client             as_client,
    output globals::AppleBus_write  ab_write,
    output logic                    smartport_active,
    output logic                    disk2_active,
    output logic [2:0]              boot_slot,
    output logic                    boot_slot_valid,
    output logic                    apple_vblank_start_pulse,
    // Aux-slot probe (boot ROM CMD $26 / report $30-$31). The pulse
    // makes apple_top drop aux_provide the same cycle, so the ROM's
    // write/read test sees the raw slot even on a warm reboot.
    output logic                    aux_probe_pulse,
    output logic [1:0]              aux_status, // {report_valid, card_present}
    input  logic                    aux_status_clear  // PS consumed the report
);

    localparam logic [7:0] BM_REG_STATUS        = 8'h00;
    localparam logic [7:0] BM_REG_CONTROL       = 8'h01;
    localparam logic [7:0] BM_REG_TIMEOUT_TICKS = 8'h02;
    localparam logic [7:0] BM_REG_KEYDATA       = 8'h03;
    localparam logic [7:0] BM_REG_HANDOFF_MODE  = 8'h04;
    localparam logic [7:0] BM_REG_APPLE_STATUS  = 8'h05;
    localparam logic [7:0] BM_REG_APPLE_TIMING  = 8'h06;
    localparam logic [7:0] BM_REG_C8_PATCH      = 8'h07;

    localparam logic [1:0] SLOT_MODE_BOOTMENU  = 2'd0;
    localparam logic [1:0] SLOT_MODE_SMARTPORT = 2'd1;

    localparam logic [1:0] SLOT7_HANDOFF_SMARTPORT = 2'd1;
    localparam logic [1:0] SLOT7_HANDOFF_DISK2     = 2'd2;

    localparam logic [7:0] APPLE_REG_CMD_STATUS = 8'h0;
    localparam logic [7:0] APPLE_REG_KEY        = 8'h1;

    localparam logic [7:0] CMD_WINDOW_BEGIN = 8'h01;
    localparam logic [7:0] CMD_WINDOW_END   = 8'h02;
    localparam logic [7:0] CMD_MENU_REQUEST = 8'h03;
    localparam logic [7:0] CMD_MENU_CLOSED  = 8'h04;
    localparam logic [7:0] CMD_VBL_START    = 8'h05;

    localparam logic [31:0] DEFAULT_TIMEOUT_TICKS = 32'd399_000_000;
    localparam int unsigned KEY_FIFO_DEPTH = 8;
    localparam logic [3:0] KEY_FIFO_DEPTH_COUNT = 4'd8;

    // The //e reset handler reads $C061 (bit 7 = Open-Apple) right after a reset
    // to choose cold vs warm boot. We snoop that read inside a short window after
    // an Apple RESET to tell Open-Apple+Ctrl-Reset (cold boot -> return to the
    // boot menu) from a plain Ctrl-Reset (warm -> keep the current handoff so
    // DOS/ProDOS stay mapped). The window closes on the first $C061 read or this
    // many fclk ticks, whichever comes first (~50 ms safety net).
    localparam logic [15:0] OPENAPPLE_ADDR  = 16'hC061;
    localparam logic [23:0] OA_WINDOW_TICKS = 24'd5_000_000;

    logic [7:0] slot_rom [0:255];
    logic [7:0] slot_rom_reloc [0:255];

    // Expansion ROM page at $C800-$C8FF (256 bytes). Sourced from a
    // second .mem built alongside the slot ROM, padded with NOPs.
    // $C9xx, $CBxx-$CExx, and $CFxx are not decoded by us. $CFFF is the
    // standard expansion-ROM release signal across all cards; we leave
    // its bus path alone so other code can use it as intended.
    localparam int unsigned C8_ROM_BYTES = 256;
    logic [7:0] slot_c8_rom [0:C8_ROM_BYTES-1];

    // Scratch register file at $CA00-$CA0F (16 bytes). Backed by FPGA
    // FFs and only decoded while we own $C800, so the firmware can use
    // it as ordinary RAM without touching ZP or screen holes. This lets
    // the boot menu run fully transparent to the caller's address-space
    // state. Sized small (16 bytes) to keep FF cost trivial.
    localparam int unsigned C8_RAM_BYTES = 16;
    logic [7:0] slot_c8_ram [0:C8_RAM_BYTES-1];

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;

    logic [7:0]  axi_read_addr_q;
    logic [31:0] status_word;
    logic [31:0] keydata_word;
    logic [31:0] handoff_mode_word;
    logic [31:0] apple_timing_word;
    logic [31:0] timeout_ticks_q;
    logic [31:0] timer_q;

    logic [1:0] slot7_mode_q;
    logic [1:0] handoff_mode_q;
    logic handoff_pending_q;
    logic [2:0] boot_slot_q;
    logic       boot_slot_valid_q;
    logic        oa_window_q;        // Open-Apple snoop window armed (post-reset)
    logic [23:0] oa_window_timer_q;  // window timeout countdown (fclk ticks)
    logic apple_res_prev_q;
    logic apple_reset_release;
    logic boot_eligible_q;
    logic window_active_q;
    logic menu_requested_q;
    logic menu_close_requested_q;
    logic menu_active_ack_q;
    logic ps_close_requested_q;
    logic timeout_expired_q;
    logic key_overflow_q;
    logic [7:0] last_event_q;
    /* Machine identification reported by the boot ROM: command byte
     * $2i latches i here. 0 = not yet reported. Ids: 1=II/II+, 2=IIe,
     * 3=enhanced IIe, 4=IIgs, 5=IIc. Deliberately NOT cleared by Apple
     * warm reset or reset-release: the boot ROM only re-runs on cold
     * boots, and the machine cannot physically change without cycling
     * card power (which resets the PL and clears this). */
    logic [3:0] machine_id_q;
    logic       aux_present_q;
    logic       aux_report_valid_q;
    assign aux_status = {aux_report_valid_q, aux_present_q};

    logic [7:0] key_fifo_q [0:KEY_FIFO_DEPTH-1];
    logic [2:0] key_rd_ptr_q;
    logic [2:0] key_wr_ptr_q;
    logic [3:0] key_count_q;
    logic [7:0] key_seq_q;

    logic slot_rom_access;
    logic slot_io_access;
    logic slot_c8_rom_access;
    logic slot_c8_ram_access;
    logic slot_rom_read_hit;
    logic slot_io_read_hit;
    logic slot_io_write_hit;
    logic slot_c8_rom_read_hit;
    logic slot_c8_ram_read_hit;
    logic slot_c8_ram_write_hit;
    logic slot_c8_owner;
    logic slot_resolved;
    logic [2:0] resolved_slot;
    logic [2:0] rom_addr_slot;
    logic [2:0] io_addr_slot;
    logic [3:0] slot_io_reg_idx;
    logic [7:0] c8_rom_offset;
    logic [3:0] c8_ram_offset;
    logic [7:0] apple_status_byte;
    logic       apple_cmd_write_hit;
    logic       menu_request_write;
    logic       push_key;
    logic       pop_key;
    logic       c8_patch_write;
    logic       handoff_entry_read;
    logic       handoff_smartport;
    logic       handoff_disk2;
    logic [2:0] boot_target_slot;
    logic       boot_menu_runtime_active;

    initial begin
        $readmemh("boot_menu_slot7.mem", slot_rom);
        $readmemh("boot_menu_slot7_reloc.mem", slot_rom_reloc);
        $readmemh("boot_menu_slot7_c8.mem", slot_c8_rom);
    end

    function automatic logic [7:0] slot_rom_byte(input logic [7:0] addr,
                                                 input logic [2:0] slot);
        if (addr == 8'h07) begin
            // Advertise the boot menu as a Disk II-style ROM so every
            // Apple II autostart path can execute it from slot 7.
            slot_rom_byte = 8'h3C;
        end else begin
            slot_rom_byte = slot_rom_reloc[addr][0] ? {4'hC, 1'b0, slot}
                                                    : slot_rom[addr];
        end
    endfunction

    assign ab_write = ab_write_q;
    assign boot_slot = boot_slot_q;
    assign boot_slot_valid = boot_slot_valid_q;
    assign handoff_disk2 =
        disk2_enabled && (handoff_mode_q == SLOT7_HANDOFF_DISK2);
    assign handoff_smartport = !handoff_disk2;
    assign boot_target_slot = handoff_disk2 ? 3'h6 : 3'h7;
    assign boot_menu_runtime_active =
        (slot7_mode_q == SLOT_MODE_BOOTMENU) &&
        (boot_eligible_q ||
         window_active_q ||
         menu_requested_q ||
         menu_active_ack_q ||
         handoff_pending_q ||
         boot_slot_valid_q);
    assign smartport_active =
        (slot7_mode_q == SLOT_MODE_SMARTPORT) ||
        (handoff_pending_q && handoff_smartport && handoff_entry_read);
    assign disk2_active =
        disk2_enabled &&
        ((slot7_mode_q == SLOT_MODE_SMARTPORT) ||
         (handoff_pending_q && handoff_disk2 && handoff_entry_read));
    assign apple_reset_release = !apple_res_prev_q && ab_read.res;
    assign pop_key = as_client.awvalid &&
                     (as_common.awaddr == BM_REG_CONTROL) &&
                     as_common.wstrb[0] &&
                     as_common.wdata[4];
    assign c8_patch_write = as_client.awvalid &&
                            (as_common.awaddr == BM_REG_C8_PATCH) &&
                            as_common.wstrb[0] &&
                            as_common.wstrb[1];
    assign push_key = slot_io_write_hit &&
                      (slot_io_reg_idx == APPLE_REG_KEY[3:0]) &&
                      boot_eligible_q &&
                      menu_requested_q;

    always_comb begin
        slot_resolved = boot_slot_valid_q;
        resolved_slot = boot_slot_q;
        rom_addr_slot = ab_read.addr[10:8];
        io_addr_slot = ab_read.addr[6:4];
        handoff_entry_read = handoff_pending_q &&
                             slot_resolved &&
                             ab_read.serve_en &&
                             ab_read.rw &&
                             sss.slot_access &&
                             (ab_read.addr[15:12] == 4'hC) &&
                             (ab_read.addr[11] == 1'b0) &&
                             (rom_addr_slot == boot_target_slot) &&
                             (ab_read.addr[7:0] == 8'h00);
        slot_rom_access = (slot7_mode_q == SLOT_MODE_BOOTMENU) &&
                          !handoff_entry_read &&
                          sss.slot_access &&
                          (ab_read.addr[15:12] == 4'hC) &&
                          (ab_read.addr[11] == 1'b0) &&
                          (rom_addr_slot != 3'h0) &&
                          (slot_resolved ? (rom_addr_slot == resolved_slot) :
                                           (rom_addr_slot == 3'h7));
        slot_io_access = (slot7_mode_q == SLOT_MODE_BOOTMENU) &&
                         !handoff_pending_q &&
                         (ab_read.addr[15:8] == 8'hC0) &&
                         ab_read.addr[7] &&
                         (io_addr_slot != 3'h0) &&
                         (slot_resolved ? (io_addr_slot == resolved_slot) :
                                          (io_addr_slot == 3'h7));
        // Expansion-ROM ownership gate. Reads in the $C800-$CFFE
        // region are only driven when the soft-switch manager has
        // latched our slot as the C8 owner -- otherwise we'd contend
        // with whichever card actually owns C8. We never serve $CFFF
        // (the standard release-signal address).
        // INTCXROM=1 hands all of $C100-$CFFF to motherboard internal
        // ROM regardless of a latched claim; driving then contends with
        // the motherboard (enhanced //e monitor listing crashes).
        slot_c8_owner = (slot7_mode_q == SLOT_MODE_BOOTMENU) &&
                        !handoff_pending_q &&
                        slot_resolved &&
                        !sss.sw_intcxrom &&
                        sss.io_select[resolved_slot] &&
                        (ab_read.addr[15:12] == 4'hC) &&
                        (ab_read.addr[11] == 1'b1) &&
                        (ab_read.addr[10:0] != 11'h7FF);

        // ROM page at $C800-$C8FF (read-only). Gated on ownership.
        slot_c8_rom_access = slot_c8_owner &&
                             (ab_read.addr[10:8] == 3'b000);

        // RAM register file at $CA00-$CA0F. Decoded purely on address
        // match -- ownership is *not* required for either reads or
        // writes. The firmware's slot_setup must populate
        // SLOT16/SLOTRET_HI/SLOT_CN *before* slot_resolved gets
        // latched (slot_resolved itself depends on SLOT16 being
        // populated, since the CMD_WINDOW_BEGIN write that sets it
        // goes through `LDY SLOT16; STA $C080,Y`). Gating on
        // ownership would deadlock that bootstrap chain. Driving
        // $CAxx reads unconditionally is safe in this codebase
        // because nothing else decodes $CAxx, so there's no bus
        // contention to worry about -- and the menu code only ever
        // reads back what it itself just wrote.
        slot_c8_ram_access = (slot7_mode_q == SLOT_MODE_BOOTMENU) &&
                             !sss.sw_intcxrom &&
                             !sss.c8_internal_rom &&
                             (ab_read.addr[15:12] == 4'hC) &&
                             (ab_read.addr[11] == 1'b1) &&
                             (ab_read.addr[10:8] == 3'b010) &&
                             (ab_read.addr[7:4] == 4'h0);

        slot_rom_read_hit = ab_read.serve_en &&
                            ab_read.rw && slot_rom_access;
        slot_io_read_hit = ab_read.serve_en &&
                           ab_read.rw && slot_io_access;
        slot_io_write_hit = ab_read.data_en && !ab_read.rw && slot_io_access;
        slot_c8_rom_read_hit = ab_read.serve_en &&
                               ab_read.rw && slot_c8_rom_access;
        slot_c8_ram_read_hit = ab_read.serve_en &&
                               ab_read.rw && slot_c8_ram_access;
        slot_c8_ram_write_hit = ab_read.data_en && !ab_read.rw && slot_c8_ram_access;
        slot_io_reg_idx = ab_read.addr[3:0];
        c8_rom_offset = ab_read.addr[7:0];
        c8_ram_offset = ab_read.addr[3:0];
        apple_cmd_write_hit = slot_io_write_hit &&
                              (slot_io_reg_idx == APPLE_REG_CMD_STATUS[3:0]);
        menu_request_write = apple_cmd_write_hit &&
                             (ab_read.data == CMD_MENU_REQUEST) &&
                             boot_eligible_q;
        aux_probe_pulse = apple_cmd_write_hit && (ab_read.data == 8'h26);

        apple_status_byte = {
            2'd0,
            ps_close_requested_q,
            handoff_disk2,
            handoff_smartport,
            boot_eligible_q,
            menu_active_ack_q,
            timeout_expired_q || !boot_eligible_q
        };

        ab_write_d = ab_write_q;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        ab_write_d.assert_irq = 1'b0;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (ab_read.serve_en) begin
            if (slot_rom_read_hit) begin
                ab_write_d.wr_data = slot_rom_byte(
                    ab_read.addr[7:0],
                    slot_resolved ? resolved_slot : rom_addr_slot
                );
                ab_write_d.wr_data_en = 1'b1;
            end else if (slot_io_read_hit) begin
                case (slot_io_reg_idx)
                    APPLE_REG_CMD_STATUS[3:0]: ab_write_d.wr_data = apple_status_byte;
                    default:                   ab_write_d.wr_data = 8'h00;
                endcase
                ab_write_d.wr_data_en = 1'b1;
            end else if (slot_c8_rom_read_hit) begin
                ab_write_d.wr_data = slot_c8_rom[c8_rom_offset];
                ab_write_d.wr_data_en = 1'b1;
            end else if (slot_c8_ram_read_hit) begin
                if (handoff_pending_q && c8_ram_offset == 4'h2) begin
                    ab_write_d.wr_data = {4'hC, 1'b0, boot_target_slot};
                end else if (handoff_pending_q && c8_ram_offset == 4'h1) begin
                    ab_write_d.wr_data = {4'hC, 1'b0, boot_target_slot} - 8'h01;
                end else begin
                    ab_write_d.wr_data = slot_c8_ram[c8_ram_offset];
                end
                ab_write_d.wr_data_en = 1'b1;
            end else begin
                ab_write_d.wr_data = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
            end
        end else if (ab_read.data_en) begin
            ab_write_d.wr_data = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end

        status_word = {
            last_event_q,
            key_seq_q,
            machine_id_q,
            key_count_q,
            key_overflow_q,
            (key_count_q != 4'd0),
            menu_active_ack_q,
            timeout_expired_q,
            menu_close_requested_q,
            menu_requested_q,
            window_active_q,
            boot_eligible_q
        };

        keydata_word = {
            key_seq_q,
            12'd0,
            key_count_q,
            (key_count_q != 4'd0) ? key_fifo_q[key_rd_ptr_q] : 8'h00
        };
        handoff_mode_word = {
            24'd0,
            handoff_pending_q,
            1'b0,
            slot7_mode_q,
            handoff_mode_q
        };
        apple_timing_word = {
            30'd0,
            apple_video_mode_50hz,
            apple_video_mode_valid
        };
    end

    always_ff @(posedge clk) begin
        integer i;

        if (!rstn) begin
            ab_write_q <= '0;
            apple_vblank_start_pulse <= 1'b0;
            axi_read_addr_q <= 8'h00;
            timeout_ticks_q <= DEFAULT_TIMEOUT_TICKS;
            timer_q <= 32'd0;
            slot7_mode_q <= SLOT_MODE_BOOTMENU;
            handoff_mode_q <= SLOT7_HANDOFF_SMARTPORT;
            handoff_pending_q <= 1'b0;
            boot_slot_q <= 3'h0;
            boot_slot_valid_q <= 1'b0;
            apple_res_prev_q <= 1'b1;
            boot_eligible_q <= 1'b0;
            oa_window_q <= 1'b0;
            oa_window_timer_q <= 24'd0;
            window_active_q <= 1'b0;
            menu_requested_q <= 1'b0;
            menu_close_requested_q <= 1'b0;
            menu_active_ack_q <= 1'b0;
            ps_close_requested_q <= 1'b0;
            timeout_expired_q <= 1'b0;
            key_overflow_q <= 1'b0;
            last_event_q <= 8'h00;
            key_rd_ptr_q <= 3'd0;
            key_wr_ptr_q <= 3'd0;
            key_count_q <= 4'd0;
            key_seq_q <= 8'd0;
            machine_id_q <= 4'd0;
            aux_present_q <= 1'b0;
            aux_report_valid_q <= 1'b0;
            for (i = 0; i < KEY_FIFO_DEPTH; i = i + 1) begin
                key_fifo_q[i] <= 8'h00;
            end
            for (i = 0; i < C8_RAM_BYTES; i = i + 1) begin
                slot_c8_ram[i] <= 8'h00;
            end
        end else begin
            ab_write_q <= ab_write_d;
            apple_vblank_start_pulse <= 1'b0;
            axi_read_addr_q <= as_common.araddr;
            apple_res_prev_q <= ab_read.res;

            if (slot_c8_ram_write_hit) begin
                slot_c8_ram[c8_ram_offset] <= ab_read.data;
            end

            if (c8_patch_write) begin
                slot_c8_rom[as_common.wdata[15:8]] <= as_common.wdata[7:0];
            end

            if (!ab_read.res) begin
                // Warm Apple RESET (Ctrl-Reset) held. A real Disk II / SmartPort
                // is NOT unplugged by RESET, so DO NOT revert the handoff here:
                // leave slot7_mode_q / handoff_pending_q intact so the storage
                // cards stay mapped and DOS/ProDOS warm-start and keep reading.
                // Only the transient boot-menu / key runtime state is cleared.
                boot_slot_q <= 3'h0;
                boot_slot_valid_q <= 1'b0;
                boot_eligible_q <= 1'b0;
                window_active_q <= 1'b0;
                menu_requested_q <= 1'b0;
                menu_close_requested_q <= 1'b0;
                menu_active_ack_q <= 1'b0;
                ps_close_requested_q <= 1'b0;
                timeout_expired_q <= 1'b0;
                timer_q <= 32'd0;
                key_overflow_q <= 1'b0;
                last_event_q <= 8'h00;
                key_rd_ptr_q <= 3'd0;
                key_wr_ptr_q <= 3'd0;
                key_count_q <= 4'd0;
                key_seq_q <= 8'd0;
            end else if (apple_reset_release) begin
                if (slot7_mode_q == SLOT_MODE_BOOTMENU) begin
                    // Cold boot with no handoff yet (e.g. power-on): the boot
                    // menu owns slot 7, so make it eligible to run.
                    boot_eligible_q <= 1'b1;
                end else begin
                    // Already handed off => this is a warm Ctrl-Reset. Keep the
                    // handoff (storage stays mapped) and arm Open-Apple detection
                    // so Open-Apple+Ctrl-Reset can still cold-boot back to the
                    // menu (decided in the window below).
                    oa_window_q <= 1'b1;
                    oa_window_timer_q <= OA_WINDOW_TICKS;
                end
                boot_slot_q <= 3'h0;
                boot_slot_valid_q <= 1'b0;
                window_active_q <= 1'b0;
                menu_requested_q <= 1'b0;
                menu_close_requested_q <= 1'b0;
                menu_active_ack_q <= 1'b0;
                ps_close_requested_q <= 1'b0;
                timeout_expired_q <= 1'b0;
                timer_q <= 32'd0;
                key_overflow_q <= 1'b0;
                last_event_q <= 8'h00;
                key_rd_ptr_q <= 3'd0;
                key_wr_ptr_q <= 3'd0;
                key_count_q <= 4'd0;
                key_seq_q <= 8'd0;
            end else begin
                // Open-Apple (cold-boot) detection window. The //e reset handler
                // reads $C061 (bit 7 = Open-Apple) just after reset; snoop it.
                // Open-Apple held => cold boot => return to the boot menu. The
                // window closes on the first $C061 read (or a timeout), so a
                // later game read of $C061 can never trigger a spurious menu.
                if (oa_window_q) begin
                    if (ab_read.data_en && ab_read.rw &&
                        (ab_read.addr == OPENAPPLE_ADDR)) begin
                        oa_window_q <= 1'b0;
                        if (ab_read.data[7]) begin
                            slot7_mode_q      <= SLOT_MODE_BOOTMENU;
                            handoff_pending_q <= 1'b0;
                            boot_eligible_q   <= 1'b1;
                        end
                    end else if (oa_window_timer_q == 24'd0) begin
                        oa_window_q <= 1'b0;
                    end else begin
                        oa_window_timer_q <= oa_window_timer_q - 24'd1;
                    end
                end

                if (handoff_entry_read) begin
                    slot7_mode_q <= SLOT_MODE_SMARTPORT;
                    handoff_pending_q <= 1'b0;
                end

                if (window_active_q && !timeout_expired_q && !menu_request_write) begin
                    if ((timeout_ticks_q == 32'd0) ||
                        (timer_q >= (timeout_ticks_q - 32'd1))) begin
                        timeout_expired_q <= 1'b1;
                        window_active_q <= 1'b0;
                        boot_eligible_q <= 1'b0;
                    end else begin
                        timer_q <= timer_q + 32'd1;
                    end
                end

                if (apple_cmd_write_hit && (ab_read.data[7:4] == 4'h2)) begin
                    machine_id_q <= ab_read.data[3:0];
                end

                if (aux_status_clear) begin
                    aux_report_valid_q <= 1'b0;
                end
                /* set wins over a same-cycle clear: a fresh report
                 * must never be lost to a stale consume */
                if (apple_cmd_write_hit &&
                    (ab_read.data == 8'h30 || ab_read.data == 8'h31)) begin
                    aux_present_q      <= ab_read.data[0];
                    aux_report_valid_q <= 1'b1;
                end

                if (apple_cmd_write_hit) begin
                    if (!boot_slot_valid_q && (ab_read.data == CMD_WINDOW_BEGIN)) begin
                        boot_slot_q <= io_addr_slot;
                        boot_slot_valid_q <= 1'b1;
                    end
                    last_event_q <= ab_read.data;
                    case (ab_read.data)
                        CMD_WINDOW_BEGIN: begin
                            if (boot_eligible_q) begin
                                window_active_q <= 1'b1;
                                menu_requested_q <= 1'b0;
                                menu_close_requested_q <= 1'b0;
                                ps_close_requested_q <= 1'b0;
                                timeout_expired_q <= 1'b0;
                                timer_q <= 32'd0;
                            end
                        end
                        CMD_WINDOW_END: begin
                            window_active_q <= 1'b0;
                            boot_eligible_q <= 1'b0;
                            handoff_pending_q <= 1'b1;
                        end
                        CMD_MENU_REQUEST: begin
                            if (boot_eligible_q) begin
                                window_active_q <= 1'b0;
                                menu_requested_q <= 1'b1;
                                menu_close_requested_q <= 1'b0;
                                ps_close_requested_q <= 1'b0;
                                timeout_expired_q <= 1'b0;
                            end
                        end
                        CMD_MENU_CLOSED: begin
                            window_active_q <= 1'b0;
                            boot_eligible_q <= 1'b0;
                            menu_requested_q <= 1'b0;
                            menu_close_requested_q <= 1'b1;
                            ps_close_requested_q <= 1'b0;
                            handoff_pending_q <= 1'b1;
                        end
                        CMD_VBL_START: begin
                            if (boot_eligible_q) begin
                                apple_vblank_start_pulse <= 1'b1;
                            end
                        end
                        default: begin
                        end
                    endcase
                end

                if (as_client.awvalid && (as_common.awaddr == BM_REG_CONTROL)) begin
                    if (as_common.wstrb[0]) begin
                        if (as_common.wdata[0]) menu_active_ack_q <= 1'b1;
                        if (as_common.wdata[1]) begin
                            menu_active_ack_q <= 1'b0;
                            ps_close_requested_q <= 1'b0;
                        end
                        if (as_common.wdata[2]) begin
                            menu_requested_q <= 1'b0;
                            ps_close_requested_q <= 1'b0;
                        end
                        if (as_common.wdata[3]) begin
                            menu_close_requested_q <= 1'b0;
                            ps_close_requested_q <= 1'b0;
                        end
                        if (as_common.wdata[5]) key_overflow_q <= 1'b0;
                        if (as_common.wdata[6]) boot_eligible_q <= 1'b0;
                        if (as_common.wdata[7]) ps_close_requested_q <= 1'b1;
                    end
                end

                if (pop_key && (key_count_q != 4'd0)) begin
                    key_rd_ptr_q <= key_rd_ptr_q + 3'd1;
                    if (!push_key) begin
                        key_count_q <= key_count_q - 4'd1;
                    end
                end

                if (push_key) begin
                    if ((key_count_q != KEY_FIFO_DEPTH_COUNT) || pop_key) begin
                        key_fifo_q[key_wr_ptr_q] <= ab_read.data;
                        key_wr_ptr_q <= key_wr_ptr_q + 3'd1;
                        key_seq_q <= key_seq_q + 8'd1;
                        if (!pop_key) begin
                            key_count_q <= key_count_q + 4'd1;
                        end
                    end else begin
                        key_overflow_q <= 1'b1;
                    end
                end
            end

            // Boot configuration must be accepted while the PS holds the
            // Apple in reset; otherwise cold-boot config writes are lost.
            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    BM_REG_TIMEOUT_TICKS: begin
                        timeout_ticks_q <= globals::apply_wstrb(
                            timeout_ticks_q,
                            as_common.wdata,
                            as_common.wstrb
                        );
                    end
                    BM_REG_HANDOFF_MODE: begin
                        if (as_common.wstrb[0]) begin
                            case (as_common.wdata[1:0])
                                SLOT7_HANDOFF_SMARTPORT:
                                    handoff_mode_q <= SLOT7_HANDOFF_SMARTPORT;
                                SLOT7_HANDOFF_DISK2:
                                    handoff_mode_q <= SLOT7_HANDOFF_DISK2;
                                default:
                                    handoff_mode_q <= SLOT7_HANDOFF_SMARTPORT;
                            endcase
                        end
                    end
                    default: begin
                    end
                endcase
            end
        end
    end

    always_comb begin
        case (axi_read_addr_q)
            BM_REG_STATUS:        as_client.rdata = status_word;
            BM_REG_TIMEOUT_TICKS: as_client.rdata = timeout_ticks_q;
            BM_REG_KEYDATA:       as_client.rdata = keydata_word;
            BM_REG_HANDOFF_MODE:  as_client.rdata = handoff_mode_word;
            BM_REG_APPLE_STATUS:  as_client.rdata = {24'h000000, apple_status_byte};
            BM_REG_APPLE_TIMING:  as_client.rdata = apple_timing_word;
            default:              as_client.rdata = 32'h0000_0000;
        endcase
    end

endmodule
