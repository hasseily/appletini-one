`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name: apple_bus_write_arbiter
//
// Data/address bus: priority mux (client 0 = highest priority). On simultaneous
//   writers, the highest-priority client's payload wins intact.
// Assert lines (inh/res/irq/rdy/nmi/dma): open-drain OR — any client may pull
//   them active, mirroring real 6502 bus behavior.
//////////////////////////////////////////////////////////////////////////////////

module apple_bus_write_arbiter #(parameter NUM_CLIENTS = 1) (
    /* Machine-mode interlock: when low, any
     * client serve that DEPENDS on INH (assert_inh + data drive in the
     * same request) is dropped WHOLE -- suppressing only the INH pin
     * while still driving data would put us in contention with the
     * uninhibited motherboard, which is exactly the failure the
     * interlock exists to prevent. IOSEL/DEVSEL-decoded serves don't
     * set assert_inh and pass through untouched. DMA is also blocked
     * whenever machine mode forbids bus mastering. */
    input  logic                                      inh_allowed,
    input  globals::AppleBus_write [NUM_CLIENTS-1:0] client_writes,
    output globals::AppleBus_write                    ab_write
);

    /* Per-client effective requests after the interlock. */
    globals::AppleBus_write [NUM_CLIENTS-1:0] gated_writes;

    always_comb begin
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            gated_writes[i] = client_writes[i];
            if (!inh_allowed) begin
                if (client_writes[i].assert_inh) begin
                    /* INH-dependent serve: drop it entirely. */
                    gated_writes[i].assert_inh     = 1'b0;
                    gated_writes[i].wr_data_en     = 1'b0;
                    gated_writes[i].wr_addr_rw_en  = 1'b0;
                end
                gated_writes[i].assert_dma = 1'b0;
            end
        end
    end

    // ---- Priority mux for data + address bus ----
    always_comb begin
        ab_write.wr_data       = '0;
        ab_write.wr_data_en    = 1'b0;
        ab_write.wr_addr       = '0;
        ab_write.wr_rw         = 1'b0;
        ab_write.wr_addr_rw_en = 1'b0;
        for (int i = NUM_CLIENTS-1; i >= 0; i--) begin
            if (gated_writes[i].wr_data_en) begin
                ab_write.wr_data    = gated_writes[i].wr_data;
                ab_write.wr_data_en = 1'b1;
            end
            if (gated_writes[i].wr_addr_rw_en) begin
                ab_write.wr_addr       = gated_writes[i].wr_addr;
                ab_write.wr_rw         = gated_writes[i].wr_rw;
                ab_write.wr_addr_rw_en = 1'b1;
            end
        end
    end

    // ---- Open-drain OR for assert lines ----
    always_comb begin
        ab_write.assert_inh = 1'b0;
        ab_write.assert_res = 1'b0;
        ab_write.assert_irq = 1'b0;
        ab_write.assert_rdy = 1'b0;
        ab_write.assert_nmi = 1'b0;
        ab_write.assert_dma = 1'b0;
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            ab_write.assert_inh |= gated_writes[i].assert_inh;
            ab_write.assert_res |= client_writes[i].assert_res;
            ab_write.assert_irq |= client_writes[i].assert_irq;
            ab_write.assert_rdy |= client_writes[i].assert_rdy;
            ab_write.assert_nmi |= client_writes[i].assert_nmi;
            ab_write.assert_dma |= gated_writes[i].assert_dma;
        end
    end

endmodule
