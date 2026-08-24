`timescale 1ns / 1ps

module axisimple_wrapper(
    input	wire				S_AXI_ACLK,
    input	wire				S_AXI_ARESETN,
    //
    // Write address channel coming from upstream
    input	wire				S_AXI_AWVALID,
    output	wire				S_AXI_AWREADY,
    input	wire	[11:0]	S_AXI_AWID,
    input	wire	[31:0]	S_AXI_AWADDR,
    input	wire	[3:0]			S_AXI_AWLEN,
    input	wire	[2:0]			S_AXI_AWSIZE,
    input	wire	[1:0]			S_AXI_AWBURST,
    input	wire				S_AXI_AWLOCK,
    input	wire	[3:0]			S_AXI_AWCACHE,
    input	wire	[2:0]			S_AXI_AWPROT,
    input	wire	[3:0]			S_AXI_AWQOS,
    //
    // Write data channel coming from upstream
    input	wire				S_AXI_WVALID,
    output	wire				S_AXI_WREADY,
    input	wire	[31:0]	S_AXI_WDATA,
    input	wire [3:0]	S_AXI_WSTRB,
    input	wire 				S_AXI_WLAST,
    //
    // Write responses sent back
    output	wire				S_AXI_BVALID,
    input	wire				S_AXI_BREADY,
    output	wire	[11:0]	S_AXI_BID,
    output	wire	[1:0]			S_AXI_BRESP,
    //
    // Read address request channel from upstream
    input	wire				S_AXI_ARVALID,
    output	wire				S_AXI_ARREADY,
    input	wire	[11:0]	S_AXI_ARID,
    input	wire	[31:0]	S_AXI_ARADDR,
    input	wire	[3:0]			S_AXI_ARLEN,
    input	wire	[2:0]			S_AXI_ARSIZE,
    input	wire	[1:0]			S_AXI_ARBURST,
    input	wire				S_AXI_ARLOCK,
    input	wire	[3:0]			S_AXI_ARCACHE,
    input	wire	[2:0]			S_AXI_ARPROT,
    input	wire	[3:0]			S_AXI_ARQOS,
    //
    // Read data return channel back upstream
    output	wire				S_AXI_RVALID,
    input	wire				S_AXI_RREADY,
    output	wire	[11:0]	S_AXI_RID,
    output	wire	[31:0]	S_AXI_RDATA,
    output	wire				S_AXI_RLAST,
    output	wire	[1:0]			S_AXI_RRESP,

    output wire [31:0] as_vtw_phasor_wdata,
    output wire [3:0]  as_vtw_phasor_wstrb,
    output globals::AxiSimple_common as_common,
    AxiSimple_if.master as_clients [7:0]
);

    // these signals are all dummy signals in the interface,
    // we are not exposing them and are explicitly silencing them
    // as unconnected.
    (* keep="soft" *) wire axid_awid_unused;
    (* keep="soft" *) wire [3:0] axid_awlen_unused;
    (* keep="soft" *) wire [2:0] axid_awsize_unused;
    (* keep="soft" *) wire [1:0] axid_awburst_unused;
    (* keep="soft" *) wire axid_awlock_unused;
    (* keep="soft" *) wire [3:0] axid_awcache_unused;
    (* keep="soft" *) wire [2:0] axid_awprot_unused;
    (* keep="soft" *) wire [3:0] axid_awqos_unused;
    (* keep="soft" *) wire [7:0] axid_wvalid_unused;
    (* keep="soft" *) wire axid_wlast_unused;
    (* keep="soft" *) wire axid_bready_unused;

    (* keep="soft" *) wire [7:0] axid_arvalid_unused;
    (* keep="soft" *) wire axid_arid_unused;
    (* keep="soft" *) wire [3:0] axid_arlen_unused;
    (* keep="soft" *) wire [2:0] axid_arsize_unused;
    (* keep="soft" *) wire [1:0] axid_arburst_unused;
    (* keep="soft" *) wire axid_arlock_unused;
    (* keep="soft" *) wire [3:0] axid_arcache_unused;
    (* keep="soft" *) wire [2:0] axid_arprot_unused;
    (* keep="soft" *) wire [3:0] axid_arqos_unused;
    (* keep="soft" *) wire axid_rready_unused;

    // special wires to slice off the top 22 and low 2 bits
    (* keep="soft" *) wire [21:0] axid_awaddr_high_unused;
    (* keep="soft" *) wire [1:0] axid_awaddr_low_unused;
    (* keep="soft" *) wire [21:0] axid_araddr_high_unused;
    (* keep="soft" *) wire [1:0] axid_araddr_low_unused;

    // these signals are actually used by the module,
    // but we are defining our access as always returning OK (0)
    wire [15:0] axid_bresp_unused = 0;
    wire [15:0] axid_rresp_unused = 0;

    wire [7:0] axi_awvalid_array;
    wire [255:0] axi_rdata_array;

    /* Break the hard-PS write-data fanout before it reaches the eight PL
     * register clients. Keep WDATA, WSTRB, and WLAST in one registered AXI
     * beat; the skid buffer retains one-beat-per-clock throughput and absorbs
     * downstream write-address stalls without changing transaction order.
     * This wrapper has never carried AXI3 WID, so GP0 must keep write-data
     * beats in write-address order and must not interleave write IDs. */
    wire        wskid_input_ready;
    wire        wskid_valid;
    wire        axid_wready;
    wire [36:0] wskid_payload;
    wire [36:0] wskid_payload_copy;

    assign S_AXI_WREADY = S_AXI_ARESETN && wskid_input_ready;

    /* Give long vTW, Phasor, and no-slot-clock routes their own same-stage
     * launch flops for the selected data bits and all write strobes.
     * Unselected payload bits remain aliases of the canonical skid output. */
    skidbuffer #(
        .OPT_LOWPOWER(1'b1),
        .OPT_OUTREG(1'b1),
        .OPT_COPY_MASK({1'b0, 4'b1111, 32'h0040_2526}),
        .DW(37)
    )
    wchannel_skid_i (
        .i_clk(S_AXI_ACLK),
        .i_reset(!S_AXI_ARESETN),
        .i_valid(S_AXI_WVALID),
        .o_ready(wskid_input_ready),
        .i_data({S_AXI_WLAST, S_AXI_WSTRB, S_AXI_WDATA}),
        .o_valid(wskid_valid),
        .i_ready(axid_wready),
        .o_data(wskid_payload),
        .o_data_copy(wskid_payload_copy)
    );

    assign as_vtw_phasor_wdata = wskid_payload_copy[31:0];
    assign as_vtw_phasor_wstrb = wskid_payload_copy[35:32];

    axidouble #(
        .C_AXI_DATA_WIDTH(32),
        .C_AXI_ADDR_WIDTH(32),
        .C_AXI_ID_WIDTH(12),
        .NS(8), // Number of Slaves (8)
        .OPT_LOWPOWER(1),
        .SLAVE_ADDR(
            {
                { 32'h40070000 },
                { 32'h40060000 },
                { 32'h40050000 },
                { 32'h40040000 },
                { 32'h40030000 },
                { 32'h40020000 },
                { 32'h40010000 },
                { 32'h40000000 }
            }
        ),
        .SLAVE_MASK(
            {
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 },
                { 32'hffff0000 }
            }
        ),
        // The GP0 clients are side-effect MMIO registers, not shared memory.
        // Firmware must not issue exclusive accesses to this register plane.
        .OPT_EXCLUSIVE_ACCESS(1'b0)
    ) axidouble_i (
        .S_AXI_ACLK(S_AXI_ACLK),
        .S_AXI_ARESETN(S_AXI_ARESETN),
        .S_AXI_AWVALID(S_AXI_AWVALID),
        .S_AXI_AWREADY(S_AXI_AWREADY),
        .S_AXI_AWID(S_AXI_AWID),
		.S_AXI_AWADDR(S_AXI_AWADDR),
		.S_AXI_AWLEN(S_AXI_AWLEN),
		.S_AXI_AWSIZE(S_AXI_AWSIZE),
		.S_AXI_AWBURST(S_AXI_AWBURST),
		.S_AXI_AWLOCK(S_AXI_AWLOCK),
		.S_AXI_AWCACHE(S_AXI_AWCACHE),
		.S_AXI_AWPROT(S_AXI_AWPROT),
		.S_AXI_AWQOS(S_AXI_AWQOS),
		.S_AXI_WVALID(wskid_valid),
		.S_AXI_WREADY(axid_wready),
		.S_AXI_WDATA(wskid_payload[31:0]),
		.S_AXI_WSTRB(wskid_payload[35:32]),
		.S_AXI_WLAST(wskid_payload[36]),
		.S_AXI_BVALID(S_AXI_BVALID),
		.S_AXI_BREADY(S_AXI_BREADY),
		.S_AXI_BID(S_AXI_BID),
		.S_AXI_BRESP(S_AXI_BRESP),
        .S_AXI_ARVALID(S_AXI_ARVALID),
		.S_AXI_ARREADY(S_AXI_ARREADY),
		.S_AXI_ARID(S_AXI_ARID),
		.S_AXI_ARADDR(S_AXI_ARADDR),
		.S_AXI_ARLEN(S_AXI_ARLEN),
		.S_AXI_ARSIZE(S_AXI_ARSIZE),
		.S_AXI_ARBURST(S_AXI_ARBURST),
		.S_AXI_ARLOCK(S_AXI_ARLOCK),
		.S_AXI_ARCACHE(S_AXI_ARCACHE),
		.S_AXI_ARPROT(S_AXI_ARPROT),
		.S_AXI_ARQOS(S_AXI_ARQOS),
        .S_AXI_RVALID(S_AXI_RVALID),
		.S_AXI_RREADY(S_AXI_RREADY),
		.S_AXI_RID(S_AXI_RID),
		.S_AXI_RDATA(S_AXI_RDATA),
		.S_AXI_RLAST(S_AXI_RLAST),
		.S_AXI_RRESP(S_AXI_RRESP),
        .M_AXI_AWVALID(axi_awvalid_array),
		.M_AXI_AWID(axid_awid_unused),
		.M_AXI_AWADDR({axid_awaddr_high_unused, as_common.awaddr, axid_awaddr_low_unused}),
		.M_AXI_AWLEN(axid_awlen_unused),
		.M_AXI_AWSIZE(axid_awsize_unused),
		.M_AXI_AWBURST(axid_awburst_unused),
		.M_AXI_AWLOCK(axid_awlock_unused),
		.M_AXI_AWCACHE(axid_awcache_unused),
		.M_AXI_AWPROT(axid_awprot_unused),
		.M_AXI_AWQOS(axid_awqos_unused),
        .M_AXI_WVALID(axid_wvalid_unused),
		.M_AXI_WDATA(as_common.wdata),
		.M_AXI_WSTRB(as_common.wstrb),
		.M_AXI_WLAST(axid_wlast_unused),
        .M_AXI_BREADY(axid_bready_unused),
		.M_AXI_BRESP(axid_bresp_unused),
		.M_AXI_ARVALID(axid_arvalid_unused),
		.M_AXI_ARID(axid_arid_unused),
		.M_AXI_ARADDR({axid_araddr_high_unused, as_common.araddr, axid_araddr_low_unused}),
		.M_AXI_ARLEN(axid_arlen_unused),
		.M_AXI_ARSIZE(axid_arsize_unused),
		.M_AXI_ARBURST(axid_arburst_unused),
		.M_AXI_ARLOCK(axid_arlock_unused),
		.M_AXI_ARCACHE(axid_arcache_unused),
		.M_AXI_ARPROT(axid_arprot_unused),
		.M_AXI_ARQOS(axid_arqos_unused),
        .M_AXI_RREADY(axid_rready_unused),
		.M_AXI_RDATA(axi_rdata_array),
		.M_AXI_RRESP(axid_rresp_unused)
    );

    genvar i;
    for (i = 0; i < 8; ++i) begin
        assign as_clients[i].awvalid = axi_awvalid_array[i];
        assign axi_rdata_array[i*32+31:i*32] = as_clients[i].rdata;
    end

endmodule
