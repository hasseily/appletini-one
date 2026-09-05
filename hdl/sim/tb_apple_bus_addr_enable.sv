`timescale 1ns / 1ps

module tb_apple_bus_addr_enable;
    localparam int NUM_CLIENTS = 12;
    localparam int FAST_CLIENT = 11;

    logic inh_allowed;
    globals::AppleBus_write [NUM_CLIENTS-1:0] client_writes;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write ab_write_fallback;
    int failures;

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(NUM_CLIENTS),
        .FAST_ADDR_CLIENT(FAST_CLIENT)
    ) dut (
        .inh_allowed(inh_allowed),
        .client_writes(client_writes),
        .ab_write(ab_write)
    );

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(NUM_CLIENTS)
    ) fallback_dut (
        .inh_allowed(inh_allowed),
        .client_writes(client_writes),
        .ab_write(ab_write_fallback)
    );

    task automatic clear_clients;
        client_writes = '{default: '0};
        #1;
    endtask

    task automatic check(input logic condition, input string message);
        if (!condition) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    initial begin
        failures = 0;
        inh_allowed = 1'b1;
        clear_clients();
        check(!ab_write.wr_addr_rw_en, "idle address direction");

        // Prove the factored production form matches the generic reduction
        // for every possible client-enable mask.
        for (int mask = 0; mask < (1 << NUM_CLIENTS); mask++) begin
            clear_clients();
            for (int i = 0; i < NUM_CLIENTS; i++) begin
                client_writes[i].wr_addr_rw_en = mask[i];
            end
            #1;
            check(ab_write.wr_addr_rw_en === (mask != 0),
                  $sformatf("factored OR mask %03h", mask));
            check(ab_write.wr_addr_rw_en === ab_write_fallback.wr_addr_rw_en,
                  $sformatf("factored/fallback match mask %03h", mask));
        end

        // Each client must reach the final OR, including the placed vTW input.
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            clear_clients();
            client_writes[i].wr_addr_rw_en = 1'b1;
            #1;
            check(ab_write.wr_addr_rw_en,
                  $sformatf("client %0d asserts address direction", i));
            client_writes[i].wr_addr_rw_en = 1'b0;
            #1;
            check(!ab_write.wr_addr_rw_en,
                  $sformatf("client %0d releases address direction", i));
        end

        // The INH safety gate must still drop a dependent address drive.
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            clear_clients();
            client_writes[i].assert_inh = 1'b1;
            client_writes[i].wr_addr_rw_en = 1'b1;
            client_writes[i].wr_data_en = 1'b1;
            client_writes[i].wr_dma_data_en = 1'b1;
            client_writes[i].assert_dma = 1'b1;
            inh_allowed = 1'b0;
            #1;
            check(!ab_write.assert_inh && !ab_write.assert_dma &&
                  !ab_write.wr_addr_rw_en && !ab_write.wr_data_en &&
                  !ab_write.wr_dma_data_en,
                  $sformatf("client %0d unsafe tuple blocked atomically", i));
            inh_allowed = 1'b1;
            #1;
            check(ab_write.assert_inh && ab_write.assert_dma &&
                  ab_write.wr_addr_rw_en && ab_write.wr_data_en &&
                  ab_write.wr_dma_data_en,
                  $sformatf("client %0d safe tuple restored", i));

            // Each ownership field must close the whole request even if a
            // faulty client omits DMA# or presents its tuple out of order.
            clear_clients();
            inh_allowed = 1'b0;
            client_writes[i].wr_data_en = 1'b1;
            client_writes[i].wr_addr_rw_en = 1'b1;
            #1;
            check(!ab_write.wr_addr_rw_en && !ab_write.wr_data_en,
                  $sformatf("client %0d address-only master blocked", i));
            clear_clients();
            client_writes[i].wr_data_en = 1'b1;
            client_writes[i].wr_dma_data_en = 1'b1;
            #1;
            check(!ab_write.wr_dma_data_en && !ab_write.wr_data_en,
                  $sformatf("client %0d DMA-data-only master blocked", i));
            clear_clients();
            client_writes[i].wr_data_en = 1'b1;
            #1;
            check(ab_write.wr_data_en && !ab_write.wr_addr_rw_en &&
                  !ab_write.wr_dma_data_en && !ab_write.assert_dma &&
                  !ab_write.assert_inh,
                  $sformatf("client %0d selected slave read remains enabled", i));
        end

        inh_allowed = 1'b1;

        // A handoff between vTW and another bus master must never create a
        // low pulse while either same-edge request remains asserted.
        clear_clients();
        client_writes[FAST_CLIENT].wr_addr_rw_en = 1'b1;
        #1;
        check(ab_write.wr_addr_rw_en, "vTW owns address direction");
        client_writes[7].wr_addr_rw_en = 1'b1;
        #1;
        check(ab_write.wr_addr_rw_en, "vTW and Applicard overlap");
        client_writes[FAST_CLIENT].wr_addr_rw_en = 1'b0;
        #1;
        check(ab_write.wr_addr_rw_en, "Applicard holds after vTW release");
        client_writes[7].wr_addr_rw_en = 1'b0;
        #1;
        check(!ab_write.wr_addr_rw_en, "last address owner releases");

        if (failures != 0) begin
            $fatal(1, "APPLE BUS ADDR ENABLE FAIL: %0d checks", failures);
        end
        $display("APPLE BUS ADDR ENABLE PASS");
        $finish;
    end
endmodule
