`timescale 1ns / 1ps

// Free-running Apple II bus for a future stand-alone motherboard mode.
//
// The module presents the same AppleBus_read phase contract as
// apple_bus_wrapper, but it never touches the edge-connector pins. A local
// CPU submits one request at a drive_en boundary. Slot clients consume
// AppleBus_read and return their merged AppleBus_write through ab_write.
// Idle cycles read $FFFF so the 1 MHz card and scanner cadence never stops.
module apple_virtual_bus #(
    parameter integer CYCLE_CLKS     = 130,
    parameter integer PHI0_RISE_CLK  = 65,
    parameter integer DRIVE_CLK      = 8,
    parameter integer ADDR_CLK       = 25,
    parameter integer SSS_CLK        = 26,
    parameter integer SERVE_CLK      = 73,
    parameter integer DATA_CLK       = 124
) (
    input  logic                    clk,
    input  logic                    resetn,

    // Base active-low motherboard lines. Tie high when no local source owns
    // the line. Slot assertions in ab_write pull the same virtual lines low.
    input  logic                    res_n_in,
    input  logic                    irq_n_in,
    input  logic                    nmi_n_in,
    input  logic                    rdy_n_in,
    input  logic                    dma_n_in,
    input  logic                    inh_n_in,

    // One-entry CPU request interface. req_valid must remain high until the
    // one-clock req_ready pulse. A request held by RDY repeats on the bus and
    // completes only when the virtual RDY line is released.
    input  logic                    req_valid,
    output logic                    req_ready,
    input  logic [15:0]             req_addr,
    input  logic                    req_rw,
    input  logic [7:0]              req_wdata,
    output logic                    resp_valid,
    output logic [7:0]              resp_rdata,

    // Scanner/floating-bus fallback for reads which no motherboard or slot
    // client serves.
    input  logic [7:0]              floating_bus_data,

    input  globals::AppleBus_write  ab_write,
    output globals::AppleBus_read   ab_read
);

    localparam integer PHASE_WIDTH =
        (CYCLE_CLKS <= 2) ? 1 : $clog2(CYCLE_CLKS);

    logic [PHASE_WIDTH-1:0] phase_q;
    logic                   cpu_cycle_q;
    logic [15:0]            cpu_addr_q;
    logic                   cpu_rw_q;
    logic [7:0]             cpu_wdata_q;
    logic                   cycle_slot_master_q;
    logic [15:0]            cycle_addr_q;
    logic                   cycle_rw_q;
    (* KEEP = "TRUE" *) logic [7:0] cycle_data_q;
    (* KEEP = "TRUE" *) logic       phase_data_q;

    wire phase_drive = (phase_q == PHASE_WIDTH'(DRIVE_CLK));
    wire phase_addr  = (phase_q == PHASE_WIDTH'(ADDR_CLK));
    wire phase_sss   = (phase_q == PHASE_WIDTH'(SSS_CLK));
    wire phase_serve = (phase_q == PHASE_WIDTH'(SERVE_CLK));
    wire phase_data  = phase_data_q;

    // Apple slot control lines are open drain: a high assert_* request pulls
    // the corresponding active-low line down.
    wire bus_res_n = res_n_in & ~ab_write.assert_res;
    wire bus_irq_n = irq_n_in & ~ab_write.assert_irq;
    wire bus_nmi_n = nmi_n_in & ~ab_write.assert_nmi;
    wire bus_rdy_n = rdy_n_in & ~ab_write.assert_rdy;
    wire bus_dma_n = dma_n_in & ~ab_write.assert_dma;
    wire bus_inh_n = inh_n_in & ~ab_write.assert_inh;

    // A slot DMA master has the address and data buses whenever it enables
    // the address/R-W drivers. It may begin driving just after drive_en, so
    // selection remains live through the later sample phases.
    wire        slot_master_live = ab_write.wr_addr_rw_en;
    wire [15:0] cycle_addr_live = slot_master_live ? ab_write.wr_addr :
                                    cpu_cycle_q ? cpu_addr_q : 16'hFFFF;
    wire        cycle_rw_live = slot_master_live ? ab_write.wr_rw :
                                  cpu_cycle_q ? cpu_rw_q : 1'b1;
    wire        slot_data_drive = ab_write.wr_data_en |
                                  ab_write.wr_dma_data_en;
    wire [7:0]  bus_data_live = slot_data_drive ? ab_write.wr_data :
                                (cpu_cycle_q && !cpu_rw_q &&
                                 !cycle_slot_master_q) ?
                                    cpu_wdata_q : floating_bus_data;

    // Do not start a CPU cycle while reset, RDY, or DMA holds the processor.
    // A pending RDY-stalled request also blocks the single-entry interface.
    always_comb begin
        req_ready = phase_drive && !cpu_cycle_q && bus_res_n && bus_rdy_n &&
                    bus_dma_n && !slot_master_live;
    end

    // The field values match an Enhanced //e bus. M2SEL qualification does
    // not apply, but cycle_valid remains set so every idle cycle still clocks
    // card timers and the scanner.
    always_comb begin
        ab_read             = '0;
        // Keep write bytes and floating-bus state live before the data phase:
        // several slot cards inspect them at serve_en. The registered branch
        // is selected only for the public data window.
        ab_read.data        = phase_data_q ? cycle_data_q : bus_data_live;
        ab_read.addr        = cycle_addr_q;
        ab_read.rw          = cycle_rw_q;
        ab_read.phi0        = (phase_q >= PHASE_WIDTH'(PHI0_RISE_CLK));
        ab_read.m2sel       = 1'b0;
        ab_read.m2b0        = 1'b0;
        ab_read.cycle_valid = 1'b1;
        ab_read.inh         = bus_inh_n;
        ab_read.res         = bus_res_n;
        ab_read.irq         = bus_irq_n;
        ab_read.rdy         = bus_rdy_n;
        ab_read.dma         = bus_dma_n;
        ab_read.nmi         = bus_nmi_n;
        ab_read.data_en     = phase_data;
        ab_read.addr_en     = phase_addr;
        ab_read.sss_en      = phase_sss;
        ab_read.serve_en    = phase_serve;
        ab_read.drive_en    = phase_drive;
        ab_read.addr_early  = cycle_addr_q;
        ab_read.rw_early    = cycle_rw_q;
    end

    always_ff @(posedge clk) begin
        if (!resetn) begin
            phase_q      <= '0;
            cpu_cycle_q  <= 1'b0;
            cpu_addr_q   <= 16'hFFFF;
            cpu_rw_q     <= 1'b1;
            cpu_wdata_q  <= '0;
            cycle_slot_master_q <= 1'b0;
            cycle_addr_q <= 16'hFFFF;
            cycle_rw_q   <= 1'b1;
            cycle_data_q <= '0;
            phase_data_q <= 1'b0;
            resp_valid   <= 1'b0;
            resp_rdata   <= '0;
        end else begin
            resp_valid <= 1'b0;
            phase_data_q <= (phase_q == PHASE_WIDTH'(DATA_CLK - 1));

            if (phase_q == PHASE_WIDTH'(CYCLE_CLKS - 1)) begin
                phase_q <= '0;
            end else begin
                phase_q <= phase_q + PHASE_WIDTH'(1);
            end

            // Resolve the current owner once per fabric clock after drive_en,
            // then freeze the tuple before addr_en. This gives registered bus
            // masters time to react to drive_en and also covers vTW's two-step
            // posted-write fetch. The held tuple removes the live arbiter mux
            // from every slot decoder for the rest of the native cycle.
            if ((phase_q >= PHASE_WIDTH'(DRIVE_CLK + 1)) &&
                (phase_q < PHASE_WIDTH'(ADDR_CLK))) begin
                cycle_slot_master_q <= slot_master_live;
                cycle_addr_q        <= cycle_addr_live;
                cycle_rw_q          <= cycle_rw_live;
            end

            // Cards have the full serve-to-data window to produce a response.
            // Capture the resolved byte one fabric clock before data_en, then
            // hold it across the public data phase. This keeps late registered
            // card replies intact while removing the live card arbiter from
            // every data-phase consumer.
            if (phase_q == PHASE_WIDTH'(DATA_CLK - 1)) begin
                cycle_data_q <= bus_data_live;
            end

            if (!bus_res_n) begin
                cpu_cycle_q <= 1'b0;
            end else begin
                if (req_valid && req_ready) begin
                    cpu_cycle_q <= 1'b1;
                    cpu_addr_q  <= req_addr;
                    cpu_rw_q    <= req_rw;
                    cpu_wdata_q <= req_wdata;
                end

                // A DMA address owner defers the pending CPU cycle. RDY also
                // repeats it. Once both release, data_en completes the cycle.
                if (phase_data && cpu_cycle_q && !cycle_slot_master_q &&
                    bus_rdy_n) begin
                    resp_valid  <= 1'b1;
                    resp_rdata  <= cycle_data_q;
                    cpu_cycle_q <= 1'b0;
                end
            end
        end
    end

endmodule
