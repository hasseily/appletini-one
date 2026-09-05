`timescale 1ns / 1ps

/* Remove every output from a card as soon as direct slot policy disables it.
 * This includes latched IRQ requests that may remain set inside a card after
 * its bus strobes stop. */
module apple_bus_client_policy_gate #(
    parameter int NUM_CLIENTS = 1
) (
    input  logic [NUM_CLIENTS-1:0]                  client_enable,
    input  globals::AppleBus_write [NUM_CLIENTS-1:0] client_writes,
    output globals::AppleBus_write [NUM_CLIENTS-1:0] gated_writes
);

    always_comb begin
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            gated_writes[i] = client_enable[i] ? client_writes[i] : '0;
        end
    end

endmodule
