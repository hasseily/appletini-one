`timescale 1ns / 1ps

module tb_apple_slot7_devsel_guard;
    logic devsel_required;
    logic minimal_io_only;
    globals::AppleBus_read ab_read_in;
    globals::AppleBus_read physical_ab_read;
    globals::AppleBus_write supersprite_write_in;
    globals::AppleBus_write smartport_write_in;
    globals::AppleBus_write boot_menu_write_in;
    globals::AppleBus_read ab_read_out;
    globals::AppleBus_write supersprite_write_out;
    globals::AppleBus_write smartport_write_out;
    globals::AppleBus_write boot_menu_write_out;

    apple_slot7_devsel_guard dut (.*);

    int failures = 0;
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    task automatic set_c0f(input logic devsel_n);
        ab_read_in.addr = 16'hC0F0;
        ab_read_in.addr_early = 16'hC0F0;
        ab_read_in.devsel_n = devsel_n;
        ab_read_in.devsel_n_early = devsel_n;
        ab_read_in.rw = 1'b1;
        ab_read_in.cycle_valid = 1'b1;
        ab_read_in.addr_en = 1'b1;
        ab_read_in.sss_en = 1'b1;
        ab_read_in.serve_en = 1'b1;
        ab_read_in.data_en = 1'b1;
        physical_ab_read = ab_read_in;
        #1;
    endtask

    initial begin
        ab_read_in = '0;
        physical_ab_read = '0;
        supersprite_write_in = '0;
        smartport_write_in = '0;
        boot_menu_write_in = '0;
        supersprite_write_in.wr_data = 8'h53;
        supersprite_write_in.wr_data_en = 1'b1;
        supersprite_write_in.assert_irq = 1'b1;
        supersprite_write_in.assert_dma = 1'b1;
        supersprite_write_in.wr_addr_rw_en = 1'b1;
        smartport_write_in.wr_data = 8'h4C;
        smartport_write_in.wr_data_en = 1'b1;
        smartport_write_in.assert_irq = 1'b1;
        boot_menu_write_in.wr_data = 8'hB7;
        boot_menu_write_in.wr_data_en = 1'b1;
        boot_menu_write_in.assert_irq = 1'b1;

        devsel_required = 1'b1;
        minimal_io_only = 1'b1;
        set_c0f(1'b0);
        check(ab_read_out.sss_en && ab_read_out.serve_en &&
              ab_read_out.data_en,
              "selected live DEVSEL passes C0F state strobes");
        check(supersprite_write_out.wr_data_en &&
              supersprite_write_out.wr_data == 8'h53 &&
              smartport_write_out.wr_data_en &&
              smartport_write_out.wr_data == 8'h4C,
              "selected C0F polling reads remain usable before ID");
        check(!supersprite_write_out.assert_irq &&
              !supersprite_write_out.assert_dma &&
              !supersprite_write_out.wr_addr_rw_en &&
              !smartport_write_out.assert_irq &&
              !boot_menu_write_out.assert_irq,
              "minimal C0F path passes no IRQ or ownership field");

        // UNKNOWN mode cannot treat DEVSEL alone as proof. On a IIgs, a
        // high pin 39 may mark a fast or internal cycle that must stay quiet.
        ab_read_in.m2sel = 1'b1;
        physical_ab_read = ab_read_in;
        #1;
        check(!ab_read_out.sss_en && !ab_read_out.serve_en &&
              !ab_read_out.data_en,
              "unknown high-pin C0F suppresses every state/data strobe");
        check(!supersprite_write_out.wr_data_en &&
              !smartport_write_out.wr_data_en &&
              !boot_menu_write_out.wr_data_en,
              "unknown high-pin C0F cannot drive a response");
        ab_read_in.m2sel = 1'b0;
        physical_ab_read = ab_read_in;

        // Source response stays asserted to model a registered stale byte.
        // A runtime remap raises DEVSEL and must kill it at once.
        set_c0f(1'b1);
        check(!ab_read_out.sss_en && !ab_read_out.serve_en &&
              !ab_read_out.data_en,
              "unselected C0F suppresses every state/data strobe");
        check(!supersprite_write_out.wr_data_en &&
              !smartport_write_out.wr_data_en &&
              !boot_menu_write_out.wr_data_en,
              "unselected C0F clears stale registered responses");

        // Early and late selects are checked against their matching address
        // samples, so a stale prior C700 value cannot authorize C0F state.
        ab_read_in.addr = 16'hC700;
        ab_read_in.devsel_n = 1'b0;
        ab_read_in.addr_early = 16'hC0F0;
        ab_read_in.devsel_n_early = 1'b1;
        physical_ab_read = ab_read_in;
        #1;
        check(!ab_read_out.sss_en,
              "early unselected C0F is blocked despite stale late C700");

        // After ID4, C8 data is allowed by M2SEL elsewhere, but GS IRQ is
        // still suppressed because the boot-time map may become stale.
        minimal_io_only = 1'b0;
        ab_read_in.addr = 16'hC800;
        ab_read_in.addr_early = 16'hC800;
        physical_ab_read = ab_read_in;
        #1;
        check(smartport_write_out.wr_data_en,
              "known GS keeps selected C8 SmartPort data service");
        check(!supersprite_write_out.assert_irq,
              "physical GS never passes SuperSprite IRQ");

        ab_read_in.m2sel = 1'b1;
        physical_ab_read = ab_read_in;
        #1;
        check(!ab_read_out.sss_en && !ab_read_out.serve_en &&
              !ab_read_out.data_en,
              "known GS high-M2SEL cycle suppresses every card strobe");
        ab_read_in.m2sel = 1'b0;
        physical_ab_read = ab_read_in;

        set_c0f(1'b0);
        check(supersprite_write_out.wr_data_en,
              "known GS selected SuperSprite polling read works");
        set_c0f(1'b1);
        check(!supersprite_write_out.wr_data_en,
              "known GS runtime internal remap kills SuperSprite data");
        check(!supersprite_write_out.assert_irq,
              "known GS runtime internal remap cannot leave IRQ asserted");

        devsel_required = 1'b0;
        minimal_io_only = 1'b0;
        #1;
        check(supersprite_write_out.assert_irq &&
              supersprite_write_out.wr_addr_rw_en,
              "ONEe/trusted legacy path remains unchanged");

        if (failures != 0)
            $fatal(1, "APPLE SLOT7 DEVSEL GUARD FAIL: %0d checks", failures);
        $display("APPLE SLOT7 DEVSEL GUARD PASS");
        $finish;
    end
endmodule
