`timescale 1ns / 1ps

module tb_apple_slot7_devsel_guard;
    logic devsel_required;
    globals::AppleBus_read ab_read_in, physical_ab_read, ab_read_out;
    globals::AppleBus_write supersprite_write_in, smartport_write_in;
    globals::AppleBus_write boot_menu_write_in, supersprite_write_out;
    globals::AppleBus_write smartport_write_out, boot_menu_write_out;
    apple_slot7_devsel_guard dut (.*);
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) $fatal(1, "%s", message);
    endtask
    task automatic cycle(input logic [15:0] addr, input logic pin39,
                         input logic devsel_n);
        ab_read_in = '0;
        ab_read_in.addr = addr;
        ab_read_in.addr_early = addr;
        ab_read_in.devsel_n = devsel_n;
        ab_read_in.devsel_n_early = devsel_n;
        ab_read_in.m2sel = pin39;
        ab_read_in.rw = 1;
        ab_read_in.cycle_valid = 1;
        ab_read_in.sss_en = 1;
        ab_read_in.serve_en = 1;
        ab_read_in.data_en = 1;
        physical_ab_read = ab_read_in;
        #1;
    endtask
    initial begin
        supersprite_write_in = '0;
        smartport_write_in = '0;
        boot_menu_write_in = '0;
        supersprite_write_in.wr_data_en = 1;
        supersprite_write_in.assert_irq = 1;
        supersprite_write_in.wr_data = 8'h53;
        smartport_write_in.wr_data_en = 1;
        boot_menu_write_in.wr_data_en = 1;
        devsel_required = 0;
        cycle(16'hC700, 1, 1);
        check(ab_read_out.serve_en && boot_menu_write_out.wr_data_en,
              "unidentified and legacy SYNC-high boot reads pass");
        cycle(16'hC0F0, 0, 1);
        check(ab_read_out.data_en && boot_menu_write_out.wr_data_en,
              "pre-ID boot commands retain legacy address decoding");
        devsel_required = 1;
        cycle(16'hC0F0, 0, 0);
        check(ab_read_out.serve_en && ab_read_out.data_en &&
              supersprite_write_out.wr_data_en &&
              !supersprite_write_out.assert_irq,
              "GS selected device polling works while IRQ stays disabled");
        cycle(16'hC0F0, 0, 1);
        check(!ab_read_out.sss_en && !ab_read_out.serve_en &&
              !ab_read_out.data_en && !supersprite_write_out.wr_data_en &&
              !smartport_write_out.wr_data_en && !boot_menu_write_out.wr_data_en,
              "live GS remap suppresses strobes and stale device replies");
        cycle(16'hC800, 0, 1);
        check(ab_read_out.serve_en && smartport_write_out.wr_data_en,
              "DEVSEL does not suppress selected GS expansion-ROM reads");
        cycle(16'hC800, 1, 1);
        check(!ab_read_out.sss_en && !ab_read_out.serve_en &&
              !ab_read_out.data_en,
              "GS internal cycles cannot advance card state");
        cycle(16'hC700, 0, 0);
        ab_read_in.addr_early = 16'hC0F0;
        ab_read_in.devsel_n_early = 1;
        #1;
        check(!ab_read_out.sss_en,
              "early select must match the early device-I/O address");
        $display("APPLE SLOT7 DEVSEL GUARD PASS");
        $finish;
    end
endmodule