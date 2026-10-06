// Extracted without edits from frozen apple_top.sv.
// Source SHA256: 1e36135964ebf1ac005450d2a149623a1de46ab8e7158364f550a9227ece6e82
module smartport_routing_baseline (
    input globals::AppleBus_read ab_read, physical_ab_read, slot7_devsel_ab_read,
    input logic onee_enable_effective, vtw_smartport_visible, slot7_overlay_devsel_visible,
    input logic [7:0] virtual_data_phase_data,
    output globals::AppleBus_read smartport_ab_read
);
    globals::AppleBus_read data_phase_ab_read;
    function automatic globals::AppleBus_read gate_ab(
        input globals::AppleBus_read ab,
        input logic en
    );
        globals::AppleBus_read g;
        g = ab;
        if (!en) begin
            g.addr_en       = 1'b0;
            g.data_en       = 1'b0;
            g.sss_en        = 1'b0;
            g.serve_en      = 1'b0;
        end
        return g;
    endfunction
    always_comb begin
        data_phase_ab_read = ab_read;
        data_phase_ab_read.data = onee_enable_effective ?
                                  virtual_data_phase_data : physical_ab_read.data;
    end

    always_comb begin
        smartport_ab_read = gate_ab(slot7_devsel_ab_read,
            vtw_smartport_visible || slot7_overlay_devsel_visible);
        if (!vtw_smartport_visible) begin
            // Only selected C0F I/O may reach the independent text overlay.
            // Keep addr_en so a stale reply clears at the next address.
            smartport_ab_read.sss_en = 1'b0;
            if (!ab_read.cycle_valid || ab_read.m2sel ||
                ab_read.devsel_n || (ab_read.addr[15:4] != 12'hC0F)) begin
                smartport_ab_read.serve_en = 1'b0;
                smartport_ab_read.data_en = 1'b0;
            end
        end
    end


endmodule
