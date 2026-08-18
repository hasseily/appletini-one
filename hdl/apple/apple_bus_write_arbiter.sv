`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name: apple_bus_write_arbiter
//
// Data/address bus: priority mux (client 0 = highest priority). On simultaneous
//   writers, the highest-priority client's payload wins intact.
// Assert lines (inh/res/irq/rdy/nmi/dma): open-drain OR — any client may pull
//   them active, mirroring real 6502 bus behavior.
//////////////////////////////////////////////////////////////////////////////////

module apple_bus_write_arbiter #(
    parameter NUM_CLIENTS = 1,
    parameter integer FAST_DATA_CLIENT = -1,
    parameter integer FAST_ADDR_CLIENT = -1
) (
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
                    gated_writes[i].wr_dma_data_en = 1'b0;
                    gated_writes[i].wr_addr_rw_en  = 1'b0;
                end
                gated_writes[i].assert_dma = 1'b0;
            end
        end
    end

    // ---- Priority mux for data + address bus ----
    // Keep the enable reductions separate from the payload muxes. The enable
    // bits only mean "at least one client is driving"; putting them in the
    // priority loops needlessly ties the board-direction outputs to the full
    // payload selection trees.
    always_comb begin
        ab_write.wr_data       = '0;
        ab_write.wr_addr       = '0;
        ab_write.wr_rw         = 1'b0;
        for (int i = NUM_CLIENTS-1; i >= 0; i--) begin
            if (gated_writes[i].wr_data_en ||
                gated_writes[i].wr_dma_data_en) begin
                ab_write.wr_data = gated_writes[i].wr_data;
            end
            if (gated_writes[i].wr_addr_rw_en) begin
                ab_write.wr_addr = gated_writes[i].wr_addr;
                ab_write.wr_rw   = gated_writes[i].wr_rw;
            end
        end
    end

    /* Factor the INH interlock after the client reductions. This is the same
     * Boolean rule as gated_writes[i].wr_data_en, but inh_allowed now crosses
     * one final LUT instead of one gate plus the client OR tree. A fixed fast
     * client still enters that last LUT directly. */
    generate
        if ((FAST_DATA_CLIENT >= 0) &&
            (FAST_DATA_CLIENT < NUM_CLIENTS)) begin : gen_fast_data_enable
            (* keep = "true" *) logic other_noninh_wr_data_en;
            (* keep = "true" *) logic other_inh_wr_data_en;
            always_comb begin
                other_noninh_wr_data_en = 1'b0;
                other_inh_wr_data_en = 1'b0;
                for (int i = 0; i < NUM_CLIENTS; i++) begin
                    if (i != FAST_DATA_CLIENT) begin
                        other_noninh_wr_data_en |=
                            client_writes[i].wr_data_en &&
                            !client_writes[i].assert_inh;
                        other_inh_wr_data_en |=
                            client_writes[i].wr_data_en &&
                            client_writes[i].assert_inh;
                    end
                end
                ab_write.wr_data_en =
                    (client_writes[FAST_DATA_CLIENT].wr_data_en &&
                     !client_writes[FAST_DATA_CLIENT].assert_inh) |
                    other_noninh_wr_data_en |
                    (inh_allowed &&
                     ((client_writes[FAST_DATA_CLIENT].wr_data_en &&
                       client_writes[FAST_DATA_CLIENT].assert_inh) |
                      other_inh_wr_data_en));
            end
        end else begin : gen_normal_data_enable
            logic noninh_wr_data_en;
            logic inh_wr_data_en;
            always_comb begin
                noninh_wr_data_en = 1'b0;
                inh_wr_data_en = 1'b0;
                for (int i = 0; i < NUM_CLIENTS; i++) begin
                    noninh_wr_data_en |= client_writes[i].wr_data_en &&
                                          !client_writes[i].assert_inh;
                    inh_wr_data_en |= client_writes[i].wr_data_en &&
                                      client_writes[i].assert_inh;
                end
                ab_write.wr_data_en = noninh_wr_data_en |
                                      (inh_allowed && inh_wr_data_en);
            end
        end
    endgenerate

    always_comb begin
        ab_write.wr_dma_data_en = 1'b0;
        for (int i = 0; i < NUM_CLIENTS; i++) begin
            ab_write.wr_dma_data_en |= gated_writes[i].wr_dma_data_en;
        end
    end

    /* The production address-direction pin is far from the vTW bus engine.
     * Keep the final, Boolean-equivalent two-input OR near that pin: vTW is
     * one input and every other client is reduced into the other. This adds
     * no state or bus phase. The fallback keeps the arbiter generic. */
    generate
        if ((FAST_ADDR_CLIENT >= 0) &&
            (FAST_ADDR_CLIENT < NUM_CLIENTS)) begin : gen_fast_addr_enable
            (* keep = "true" *) logic other_addr_rw_en;
            always_comb begin
                other_addr_rw_en = 1'b0;
                for (int i = 0; i < NUM_CLIENTS; i++) begin
                    if (i != FAST_ADDR_CLIENT) begin
                        other_addr_rw_en |= gated_writes[i].wr_addr_rw_en;
                    end
                end
            end

            (* LOC = "SLICE_X112Y23", BEL = "A6LUT", DONT_TOUCH = "TRUE" *)
            LUT2 #(.INIT(4'hE)) apple_addr_enable_lut (
                .I0(gated_writes[FAST_ADDR_CLIENT].wr_addr_rw_en),
                .I1(other_addr_rw_en),
                .O(ab_write.wr_addr_rw_en)
            );
        end else begin : gen_normal_addr_enable
            always_comb begin
                ab_write.wr_addr_rw_en = 1'b0;
                for (int i = 0; i < NUM_CLIENTS; i++) begin
                    ab_write.wr_addr_rw_en |= gated_writes[i].wr_addr_rw_en;
                end
            end
        end
    endgenerate

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
