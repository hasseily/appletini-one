`timescale 1ns / 1ps

module tb_apple_bus_client_policy_gate;

    logic [1:0] client_enable;
    globals::AppleBus_write [1:0] client_writes;
    globals::AppleBus_write [1:0] gated_writes;

    apple_bus_client_policy_gate #(.NUM_CLIENTS(2)) dut (.*);

    int failures = 0;

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    initial begin
        client_writes = '0;
        client_enable = 2'b11;
        client_writes[0].assert_irq = 1'b1;
        client_writes[0].wr_data_en = 1'b1;
        client_writes[0].wr_data = 8'hA5;
        client_writes[1].assert_nmi = 1'b1;
        #1;
        check(gated_writes[0].assert_irq &&
              gated_writes[0].wr_data_en &&
              gated_writes[0].wr_data == 8'hA5,
              "enabled card write tuple passes intact");
        check(gated_writes[1].assert_nmi,
              "second enabled card write tuple passes intact");

        // Model a same-cycle C02D or identity fault while IRQ remains
        // pending inside the disabled card. Every output must drop at once.
        client_enable[0] = 1'b0;
        #1;
        check(gated_writes[0] == '0,
              "slot-policy loss drops pending IRQ and data immediately");
        check(gated_writes[1].assert_nmi,
              "disabled client cannot disturb another enabled client");

        client_enable = '0;
        #1;
        check(gated_writes == '0,
              "fail-closed policy clears all client output tuples");

        if (failures != 0)
            $fatal(1, "APPLE BUS CLIENT POLICY GATE FAIL: %0d checks",
                   failures);
        $display("APPLE BUS CLIENT POLICY GATE PASS");
        $finish;
    end

endmodule
