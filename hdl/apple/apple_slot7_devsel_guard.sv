`timescale 1ns / 1ps

/* Final slot-7 device-I/O guard for identified IIgs physical hosts.
 *
 * /M2SEL qualifies Cn/C8 cycles. C0F device state and data also requires
 * live active-low /DEVSEL because the IIgs may remap slot I/O at run time.
 * Legacy hosts may serve virtual slot 7 from another physical slot and must
 * not use its /DEVSEL to qualify C0F. Pre-ID boot reads bypass this guard. */
module apple_slot7_devsel_guard (
    input  logic                   devsel_required,
    input  globals::AppleBus_read  ab_read_in,
    input  globals::AppleBus_read  physical_ab_read,
    input  globals::AppleBus_write supersprite_write_in,
    input  globals::AppleBus_write smartport_write_in,
    input  globals::AppleBus_write boot_menu_write_in,
    output globals::AppleBus_read  ab_read_out,
    output globals::AppleBus_write supersprite_write_out,
    output globals::AppleBus_write smartport_write_out,
    output globals::AppleBus_write boot_menu_write_out
);

    always_comb begin
        ab_read_out = ab_read_in;
        // A GS fast/internal cycle cannot change card state.
        if (devsel_required && ab_read_in.m2sel) begin
            ab_read_out.sss_en = 1'b0;
            ab_read_out.serve_en = 1'b0;
            ab_read_out.data_en = 1'b0;
        end
        if (devsel_required) begin
            if ((ab_read_in.addr_early[15:4] == 12'hC0F) &&
                ab_read_in.devsel_n_early) begin
                ab_read_out.sss_en = 1'b0;
            end
            if ((ab_read_in.addr[15:4] == 12'hC0F) &&
                ab_read_in.devsel_n) begin
                ab_read_out.data_en = 1'b0;
                ab_read_out.serve_en = 1'b0;
            end
        end

        supersprite_write_out = supersprite_write_in;
        smartport_write_out = smartport_write_in;
        boot_menu_write_out = boot_menu_write_in;

        /* A cached boot-time slot assignment cannot authorize a later IRQ
         * after GS/OS remaps slot 7 internally. Polling data remains usable
         * through live /DEVSEL; physical GS SuperSprite IRQ stays off. */
        if (devsel_required) begin
            supersprite_write_out.assert_irq = 1'b0;
        end

        if (devsel_required &&
            (physical_ab_read.addr[15:4] == 12'hC0F) &&
            physical_ab_read.devsel_n) begin
            supersprite_write_out.wr_data_en = 1'b0;
            smartport_write_out.wr_data_en = 1'b0;
            boot_menu_write_out.wr_data_en = 1'b0;
        end

    end

endmodule

/* Address ownership for the three GS peripheral windows. /M2SEL describes
 * the current bus cycle. The card's physical slot-3 /DEVSEL does not
 * describe a virtual slot's C0n window. */
module apple_iigs_slot_access (
    input  logic                  clk,
    input  logic                  resetn,
    input  globals::AppleBus_read ab_read,
    /* The wrapper's pre-sample address and its two load strobes. On a
     * sample_early_en edge the wrapper loads ab_read.addr_early from
     * sample_addr; on a sample_late_en edge it loads ab_read.addr. The
     * ownership decode registers on the same edges, so every card gate
     * sees a flop instead of a live address decode. */
    input  logic [15:0]          sample_addr,
    input  logic                 sample_early_en,
    input  logic                 sample_late_en,
    input  logic [7:0]           io_mask,
    input  logic [7:0]           rom_mask,
    input  logic [2:0]           c8_owner,
    output logic [7:0]           early_slot,
    output logic [7:0]           late_slot,
    output logic [7:0]           data_slot,
    output logic [7:0]           reply_slot
);
    function automatic logic [7:0] owned_slot(input logic [15:0] address);
        logic [2:0] slot;
        logic [7:0] result;
        result = 8'h00;
        if (address[15:7] == 9'h181) begin
            slot = address[6:4];
            result[slot] = io_mask[slot];
        end else if (address[15:11] == 5'h18 && address[10:8] != 3'd0) begin
            slot = address[10:8];
            result[slot] = rom_mask[slot];
        end else if (address[15:11] == 5'h19 && address != 16'hCFFF) begin
            result[c8_owner] = c8_owner != 3'd0 && rom_mask[c8_owner];
        end
        return result;
    endfunction

    wire qualified_cycle = ab_read.res && ab_read.cycle_valid && !ab_read.m2sel;

    /* The masks and C8 owner are registered and change only at bus
     * strobes or policy changes, never on a sample edge, so this decode
     * equals owned_slot(ab_read.addr_early / ab_read.addr) one clock
     * later. */
    logic [7:0] early_owned_q = 8'h00;
    logic [7:0] late_owned_q = 8'h00;
    always_ff @(posedge clk) begin
        if (sample_early_en)
            early_owned_q <= owned_slot(sample_addr);
        if (sample_late_en)
            late_owned_q <= owned_slot(sample_addr);
    end

    assign early_slot = qualified_cycle ? early_owned_q : 8'h00;
    assign late_slot = qualified_cycle ? late_owned_q : 8'h00;

    // Capture ownership for the later data phase at the serve edge.
    // A new address clears the tag even if /M2SEL denies the new cycle.
    logic [7:0] access_slot_q;
    always_ff @(posedge clk) begin
        if (!resetn || !ab_read.res || ab_read.addr_en || ab_read.data_en)
            access_slot_q <= 8'h00;
        else if (ab_read.serve_en)
            access_slot_q <= late_slot;
    end
    assign data_slot = (qualified_cycle && !ab_read.addr_en) ?
        access_slot_q : 8'h00;
    assign reply_slot = ab_read.rw ? data_slot : 8'h00;
endmodule
