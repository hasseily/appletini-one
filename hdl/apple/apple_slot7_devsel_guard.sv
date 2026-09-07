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
