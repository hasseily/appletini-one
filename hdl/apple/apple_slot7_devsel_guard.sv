`timescale 1ns / 1ps

/* Final slot-7 device-I/O guard for UNKNOWN/IIgs physical hosts.
 *
 * /M2SEL qualifies Cn/C8 cycles. C0F device state and data also requires
 * live active-low /DEVSEL because the IIgs may remap slot I/O at run time.
 * Before the ID4/C02D pair, optional clients may pass only that selected C0F
 * data byte; IRQ and every ownership/control request remain off. */
module apple_slot7_devsel_guard (
    input  logic                   devsel_required,
    input  logic                   minimal_io_only,
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

    wire live_c0f_read = physical_ab_read.cycle_valid &&
                         !physical_ab_read.m2sel &&
                         physical_ab_read.rw &&
                         (physical_ab_read.addr[15:4] == 12'hC0F) &&
                         !physical_ab_read.devsel_n;
    wire low_selected_boot_read = physical_ab_read.cycle_valid &&
        physical_ab_read.rw && !physical_ab_read.m2sel &&
        ((physical_ab_read.addr[15:8] == 8'hC7) ||
         ((physical_ab_read.addr >= 16'hC800) &&
          (physical_ab_read.addr < 16'hCFFF)));

    always_comb begin
        ab_read_out = ab_read_in;
        /* Before the low-only bootstrap proves a IIgs, pin 39 must also be
         * low for the small DEVSEL path. A high pin can be a IIgs fast or
         * internal cycle, so DEVSEL alone cannot authorize state or data. */
        if (devsel_required && ab_read_in.m2sel) begin
            ab_read_out.sss_en = 1'b0;
            ab_read_out.serve_en = 1'b0;
            ab_read_out.data_en = 1'b0;
        end
        if (devsel_required) begin
            if ((ab_read_in.addr_early[15:4] == 12'hC0F) &&
                (ab_read_in.devsel_n_early ||
                 (minimal_io_only && ab_read_in.m2sel))) begin
                ab_read_out.sss_en = 1'b0;
            end
            if ((ab_read_in.addr[15:4] == 12'hC0F) &&
                (ab_read_in.devsel_n ||
                 (minimal_io_only && ab_read_in.m2sel))) begin
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

        if (minimal_io_only) begin
            supersprite_write_out = '0;
            smartport_write_out = '0;
            boot_menu_write_out = '0;
            if (low_selected_boot_read || live_c0f_read) begin
                boot_menu_write_out.wr_data = boot_menu_write_in.wr_data;
                boot_menu_write_out.wr_data_en =
                    boot_menu_write_in.wr_data_en;
            end
            if (live_c0f_read) begin
                supersprite_write_out.wr_data =
                    supersprite_write_in.wr_data;
                supersprite_write_out.wr_data_en =
                    supersprite_write_in.wr_data_en;
                smartport_write_out.wr_data = smartport_write_in.wr_data;
                smartport_write_out.wr_data_en =
                    smartport_write_in.wr_data_en;
            end
        end
    end

endmodule
