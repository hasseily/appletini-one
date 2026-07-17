`timescale 1ns / 1ps

// Uthernet II-compatible Apple-side front end for a WIZnet W5100S.
//
// The hardware bus is intentionally close to passthrough: Apple data-port
// accesses normally become W5100S indirect data-port accesses. The local state
// exists only where a W5100S differs from the W5100 software model:
//   * W5100 MR IND/AI bits are Apple-visible, but W5100S indirect mode is fixed.
//   * The Apple-visible indirect address follows W5100 auto-increment/wrap rules.
//   * W5100S always auto-increments its physical indirect pointer, so the
//     physical pointer is reloaded when it is no longer known to match.
//
// Apple slot I/O:
//   $C0n0: W5100 mode register
//   $C0n1: W5100 indirect address high
//   $C0n2: W5100 indirect address low
//   $C0n3: W5100 indirect data port
//
// A0/A1 are the only decoded address bits; $C0n4-$C0nF alias the same four
// registers.
module uthernet2_card #(
    parameter int unsigned CLK_HZ = 133_000_000,
    parameter int unsigned RESET_HOLD_US = 10_000,
    parameter int unsigned RESET_READY_WAIT_US = 61_000,
    parameter int unsigned READ_STROBE_CYCLES = 16,
    parameter int unsigned WRITE_STROBE_CYCLES = 16
) (
    input  logic                    clk,
    input  logic                    rstn,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    input  logic [2:0]              slot_assign,
    output globals::AppleBus_write  ab_write,

    input  logic [7:0]              eth_d_i,
    output logic [7:0]              eth_d_o,
    output logic                    eth_d_oe,
    output logic [1:0]              eth_a,
    output logic                    eth_rd_n,
    output logic                    eth_wr_n,
    output logic                    eth_cs_n,
    output logic                    eth_rst_n,
    input  logic                    eth_int_n,

    input  logic                    host_req,
    input  logic                    host_write,
    input  logic [15:0]             host_addr,
    input  logic [7:0]              host_wdata,
    output logic                    host_ready,
    output logic                    host_done,
    output logic                    host_error,
    output logic [7:0]              host_rdata
);

    localparam int unsigned RESET_HOLD_CYCLES =
        (CLK_HZ / 1_000_000) * RESET_HOLD_US;
    localparam int unsigned RESET_READY_WAIT_CYCLES =
        (CLK_HZ / 1_000_000) * RESET_READY_WAIT_US;
    localparam int unsigned RESET_COUNTER_MAX_CYCLES =
        (RESET_HOLD_CYCLES > RESET_READY_WAIT_CYCLES) ?
            RESET_HOLD_CYCLES : RESET_READY_WAIT_CYCLES;
    localparam int unsigned RESET_COUNTER_W =
        (RESET_COUNTER_MAX_CYCLES <= 1) ? 1 : $clog2(RESET_COUNTER_MAX_CYCLES + 1);
    localparam logic [RESET_COUNTER_W-1:0] RESET_HOLD_COUNT =
        RESET_COUNTER_W'(RESET_HOLD_CYCLES);
    localparam logic [RESET_COUNTER_W-1:0] RESET_READY_WAIT_COUNT =
        RESET_COUNTER_W'(RESET_READY_WAIT_CYCLES);

    localparam int unsigned BUS_SETUP_CYCLES = 2;
    localparam int unsigned RECOVER_HOLD_CYCLES = 5;
    localparam int unsigned READ_DONE_CYCLES =
        BUS_SETUP_CYCLES + READ_STROBE_CYCLES;
    localparam int unsigned WRITE_DONE_CYCLES =
        BUS_SETUP_CYCLES + WRITE_STROBE_CYCLES;
    localparam int unsigned ETH_OP_COUNTER_MAX_CYCLES =
        (READ_DONE_CYCLES > WRITE_DONE_CYCLES) ?
            ((READ_DONE_CYCLES > RECOVER_HOLD_CYCLES) ?
                READ_DONE_CYCLES : RECOVER_HOLD_CYCLES) :
            ((WRITE_DONE_CYCLES > RECOVER_HOLD_CYCLES) ?
                WRITE_DONE_CYCLES : RECOVER_HOLD_CYCLES);
    localparam int unsigned ETH_OP_COUNTER_W =
        (ETH_OP_COUNTER_MAX_CYCLES <= 1) ? 1 : $clog2(ETH_OP_COUNTER_MAX_CYCLES + 1);
    localparam logic [ETH_OP_COUNTER_W-1:0] BUS_SETUP_COUNT =
        ETH_OP_COUNTER_W'(BUS_SETUP_CYCLES);
    localparam logic [ETH_OP_COUNTER_W-1:0] READ_DONE_COUNT =
        ETH_OP_COUNTER_W'(READ_DONE_CYCLES);
    localparam logic [ETH_OP_COUNTER_W-1:0] WRITE_DONE_COUNT =
        ETH_OP_COUNTER_W'(WRITE_DONE_CYCLES);
    localparam logic [ETH_OP_COUNTER_W-1:0] RECOVER_HOLD_COUNT =
        ETH_OP_COUNTER_W'(RECOVER_HOLD_CYCLES);

    localparam logic [1:0] U2_REG_MODE = 2'b00;
    localparam logic [1:0] U2_REG_ARH  = 2'b01;
    localparam logic [1:0] U2_REG_ARL  = 2'b10;
    localparam logic [1:0] U2_REG_DATA = 2'b11;

    localparam logic [7:0] W5100_MR_RST   = 8'h80;
    localparam logic [7:0] W5100_MR_PB    = 8'h10;
    localparam logic [7:0] W5100_MR_PPPOE = 8'h08;
    localparam logic [7:0] W5100_MR_AI    = 8'h02;
    localparam logic [7:0] W5100_MR_IND   = 8'h01;
    localparam logic [7:0] W5100S_MR_FIXED_INDIRECT =
        W5100_MR_AI | W5100_MR_IND;

    localparam logic [15:0] W5100_COMMON_REG_END = 16'h002F;
    localparam logic [15:0] W5100_SOCKET_REG_BEGIN = 16'h0400;
    localparam logic [15:0] W5100S_PHYSR   = 16'h003C;
    localparam logic [15:0] W5100S_PHYCR0  = 16'h0046;
    localparam logic [15:0] W5100S_PHYCR1  = 16'h0047;
    localparam logic [15:0] W5100S_PHYLCKR = 16'h0072;
    localparam logic [15:0] W5100S_VERR    = 16'h0080;

    typedef enum logic [3:0] {
        ETH_IDLE,
        ETH_BUS_OP,
        ETH_RECOVER,
        ETH_ADDR_LO_THEN_READ,
        ETH_ADDR_LO_THEN_WRITE,
        ETH_DATA_READ_START,
        ETH_DATA_WRITE_START,
        ETH_HOST_ADDR_RELOAD_READ,
        ETH_HOST_ADDR_RELOAD_WRITE,
        ETH_HOST_ADDR_LO_THEN_READ,
        ETH_HOST_ADDR_LO_THEN_WRITE,
        ETH_HOST_DATA_READ_START,
        ETH_HOST_DATA_WRITE_START
    } eth_state_t;

    typedef enum logic [2:0] {
        BUS_KIND_NONE,
        BUS_KIND_ADDR_HI,
        BUS_KIND_ADDR_LO,
        BUS_KIND_DATA_READ,
        BUS_KIND_DATA_WRITE,
        BUS_KIND_MODE_WRITE,
        BUS_KIND_HOST_READ,
        BUS_KIND_HOST_WRITE
    } bus_kind_t;

    globals::AppleBus_write ab_write_q;
    globals::AppleBus_write ab_write_d;

    eth_state_t eth_state_q;
    eth_state_t next_state_q;
    bus_kind_t bus_kind_q;
    logic [ETH_OP_COUNTER_W-1:0] op_cycle_q;

    logic [RESET_COUNTER_W-1:0] reset_count_q;
    logic reset_released_q;
    logic reset_done_q;

    logic read_pending_q;
    logic read_valid_q;
    logic [7:0] read_data_q;

    logic [7:0] mode_q;
    logic [15:0] data_addr_q;
    logic [15:0] phys_addr_q;
    logic addr_synced_q;

    logic [15:0] access_addr_q;
    logic [7:0] access_data_q;
    logic [7:0] payload_data_q;
    logic [1:0] bus_reg_q;
    logic bus_write_q;
    logic host_done_q;
    logic host_error_q;
    logic [7:0] host_rdata_q;
    logic host_active_q;
    /* host_req is a one-cycle pulse from apple_top. If an Apple I/O
     * access starts in that exact cycle the IDLE dispatch takes the
     * Apple branch -- without this latch the request would be lost
     * and apple_top's busy flag would wedge forever. Latch it and
     * serve it at the next free IDLE cycle instead. */
    logic host_pending_q;
    logic host_pending_write_q;
    logic [15:0] host_pending_addr_q;
    logic [7:0] host_pending_wdata_q;
    logic apple_read_pending_q;
    logic [1:0] apple_read_pending_reg_q;
    logic apple_write_reserved_q;

    wire enabled = (slot_assign != 3'h0);
    wire apple_bus_active = enabled &&
                            ((slot_assign != 3'h3) || sss.sw_slotc3rom) &&
                            ab_read.res;
    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));

    wire apple_io_read_start  = ab_read.serve_en && ab_read.cycle_valid &&
                                ab_read.rw && slot_io_hit;
    wire apple_io_write_addr_start = ab_read.serve_en && ab_read.cycle_valid &&
                                      !ab_read.rw && slot_io_hit;
    wire apple_io_write_start = ab_read.data_en && !ab_read.rw && slot_io_hit;
    wire apple_io_start = apple_io_read_start || apple_io_write_start;
    wire [1:0] apple_io_reg = ab_read.addr[1:0];
    wire apple_waiting = apple_io_start || apple_io_write_addr_start ||
                         apple_read_pending_q || apple_write_reserved_q;
    wire host_can_start =
        reset_done_q && (eth_state_q == ETH_IDLE) && !apple_waiting &&
        !host_pending_q;
    wire [15:0] host_phys_addr = host_addr & 16'h7FFF;

    wire [15:0] arh_shadow_addr = {ab_read.data, data_addr_q[7:0]};
    wire [15:0] arl_shadow_addr = {data_addr_q[15:8], ab_read.data};
    wire [15:0] arh_phys_addr = w5100_physical_addr(arh_shadow_addr);
    wire [15:0] arl_phys_addr = w5100_physical_addr(arl_shadow_addr);
    wire [15:0] data_phys_addr = w5100_physical_addr(data_addr_q);
    wire [15:0] data_shadow_next_addr = w5100_auto_inc_addr(data_addr_q);
    wire [15:0] data_phys_next_addr = access_addr_q + 16'h0001;
    wire data_access_stays_synced =
        (mode_q & W5100_MR_AI) &&
        (data_phys_next_addr == w5100_physical_addr(data_shadow_next_addr));

    assign ab_write = ab_write_q;
    assign host_ready = host_can_start;
    assign host_done = host_done_q;
    assign host_error = host_error_q;
    assign host_rdata = host_rdata_q;

    function automatic logic [15:0] w5100_physical_addr(input logic [15:0] addr);
        // Uthernet II / W5100 high mirror accesses fold into the lower 15-bit
        // W5100 space. TX remains at $4000 and RX remains at $6000.
        w5100_physical_addr = addr & 16'h7FFF;
    endfunction

    function automatic logic [15:0] w5100_auto_inc_addr(input logic [15:0] addr);
        case (addr)
            16'h5FFF: w5100_auto_inc_addr = 16'h4000;
            16'h7FFF: w5100_auto_inc_addr = 16'h6000;
            16'hFFFF: w5100_auto_inc_addr = 16'hE000;
            default:  w5100_auto_inc_addr = addr + 16'h0001;
        endcase
    endfunction

    function automatic logic [7:0] apple_local_read_data(
        input logic [1:0] reg_sel,
        input logic [7:0] mode,
        input logic [15:0] data_addr
    );
        case (reg_sel)
            U2_REG_MODE: apple_local_read_data = mode;
            U2_REG_ARH:  apple_local_read_data = data_addr[15:8];
            U2_REG_ARL:  apple_local_read_data = data_addr[7:0];
            default:     apple_local_read_data = 8'h00;
        endcase
    endfunction

    function automatic logic w5100_phy_open_addr(input logic [15:0] addr);
        w5100_phy_open_addr =
            (addr == W5100S_PHYSR)  ||
            (addr == W5100S_PHYCR0) ||
            (addr == W5100S_PHYCR1) ||
            (addr == W5100S_PHYLCKR) ||
            (addr == W5100S_VERR);
    endfunction

    function automatic logic w5100_reserved_addr(input logic [15:0] addr);
        // The W5100 has no software-visible registers in this gap. Keep it
        // hidden except for explicit W5100S diagnostic registers.
        w5100_reserved_addr =
            (addr > W5100_COMMON_REG_END) && (addr < W5100_SOCKET_REG_BEGIN) &&
            !w5100_phy_open_addr(addr);
    endfunction

    function automatic logic [7:0] next_mode_value(input logic [7:0] value);
        next_mode_value = (value & W5100_MR_RST) ? 8'h00 : value;
    endfunction

    function automatic logic [7:0] w5100s_physical_mode_value(input logic [7:0] value);
        w5100s_physical_mode_value =
            (value & (W5100_MR_RST | W5100_MR_PB | W5100_MR_PPPOE)) |
            W5100S_MR_FIXED_INDIRECT;
    endfunction

    task automatic begin_bus_op(
        input logic [1:0] reg_sel,
        input logic is_write,
        input logic [7:0] wr_data,
        input bus_kind_t kind,
        input eth_state_t after_recover
    );
        begin
            bus_reg_q <= reg_sel;
            bus_write_q <= is_write;
            access_data_q <= wr_data;
            bus_kind_q <= kind;
            next_state_q <= after_recover;
            op_cycle_q <= '0;
            eth_state_q <= ETH_BUS_OP;
        end
    endtask

    task automatic mark_shadow_data_access;
        begin
            if (mode_q & W5100_MR_AI) begin
                data_addr_q <= data_shadow_next_addr;
            end
            addr_synced_q <= 1'b0;
        end
    endtask

    task automatic mark_physical_data_access;
        begin
            if (mode_q & W5100_MR_AI) begin
                data_addr_q <= data_shadow_next_addr;
            end
            phys_addr_q <= data_phys_next_addr;
            addr_synced_q <= data_access_stays_synced;
        end
    endtask

    task automatic start_data_read;
        begin
            begin_bus_op(U2_REG_DATA, 1'b0, 8'h00, BUS_KIND_DATA_READ, ETH_IDLE);
        end
    endtask

    task automatic start_data_write(input logic [7:0] wr_data);
        begin
            begin_bus_op(U2_REG_DATA, 1'b1, wr_data, BUS_KIND_DATA_WRITE, ETH_IDLE);
        end
    endtask

    task automatic start_host_read;
        begin
            begin_bus_op(U2_REG_DATA, 1'b0, 8'h00, BUS_KIND_HOST_READ, ETH_IDLE);
        end
    endtask

    task automatic start_host_write(input logic [7:0] wr_data);
        begin
            begin_bus_op(U2_REG_DATA, 1'b1, wr_data, BUS_KIND_HOST_WRITE, ETH_IDLE);
        end
    endtask

    task automatic start_host_mode_setup(input eth_state_t after_mode);
        begin
            begin_bus_op(
                U2_REG_MODE,
                1'b1,
                w5100s_physical_mode_value(mode_q),
                BUS_KIND_MODE_WRITE,
                after_mode
            );
        end
    endtask

    task automatic start_addr_reload(
        input logic [15:0] phys_addr,
        input eth_state_t after_addr_low
    );
        begin
            access_addr_q <= phys_addr;
            begin_bus_op(
                U2_REG_ARH,
                1'b1,
                phys_addr[15:8],
                BUS_KIND_ADDR_HI,
                after_addr_low
            );
        end
    endtask

    task automatic start_apple_read(input logic [1:0] reg_sel);
        begin
            read_pending_q <= 1'b1;
            if (reg_sel != U2_REG_DATA) begin
                read_data_q <= apple_local_read_data(reg_sel, mode_q, data_addr_q);
                read_valid_q <= 1'b1;
            end else if (data_phys_addr == 16'h0000) begin
                read_data_q <= mode_q;
                read_valid_q <= 1'b1;
                mark_shadow_data_access();
            end else if (w5100_reserved_addr(data_phys_addr)) begin
                read_data_q <= 8'h00;
                read_valid_q <= 1'b1;
                mark_shadow_data_access();
            end else begin
                access_addr_q <= data_phys_addr;
                read_valid_q <= 1'b0;
                if (addr_synced_q && (phys_addr_q == data_phys_addr)) begin
                    start_data_read();
                end else begin
                    start_addr_reload(data_phys_addr, ETH_ADDR_LO_THEN_READ);
                end
            end
        end
    endtask

    task automatic fail_host_access;
        begin
            host_active_q <= 1'b0;
            host_done_q <= 1'b1;
            host_error_q <= 1'b1;
            addr_synced_q <= 1'b0;
            eth_state_q <= ETH_IDLE;
        end
    endtask

    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        ab_write_d.assert_irq = 1'b0;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (read_pending_q) begin
            ab_write_d.wr_data = read_valid_q ? read_data_q : 8'h00;
            ab_write_d.wr_data_en = 1'b1;
        end

        if (ab_read.data_en) begin
            ab_write_d.wr_data = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write_q <= '0;
            eth_state_q <= ETH_IDLE;
            next_state_q <= ETH_IDLE;
            bus_kind_q <= BUS_KIND_NONE;
            op_cycle_q <= '0;
            reset_count_q <= '0;
            reset_released_q <= 1'b0;
            reset_done_q <= 1'b0;
            read_pending_q <= 1'b0;
            read_valid_q <= 1'b0;
            read_data_q <= 8'h00;
            mode_q <= 8'h00;
            data_addr_q <= 16'h0000;
            phys_addr_q <= 16'h0000;
            addr_synced_q <= 1'b1;
            access_addr_q <= 16'h0000;
            access_data_q <= 8'h00;
            payload_data_q <= 8'h00;
            bus_reg_q <= U2_REG_MODE;
            bus_write_q <= 1'b0;
            host_done_q <= 1'b0;
            host_error_q <= 1'b0;
            host_rdata_q <= 8'h00;
            host_active_q <= 1'b0;
            host_pending_q <= 1'b0;
            host_pending_write_q <= 1'b0;
            host_pending_addr_q <= 16'h0000;
            host_pending_wdata_q <= 8'h00;
            apple_read_pending_q <= 1'b0;
            apple_read_pending_reg_q <= U2_REG_MODE;
            apple_write_reserved_q <= 1'b0;
            eth_d_o <= 8'h00;
            eth_d_oe <= 1'b0;
            eth_a <= U2_REG_MODE;
            eth_rd_n <= 1'b1;
            eth_wr_n <= 1'b1;
            eth_cs_n <= 1'b1;
            eth_rst_n <= 1'b0;
        end else begin
            ab_write_q <= ab_write_d;
            host_done_q <= 1'b0;
            host_error_q <= 1'b0;

            if (apple_io_write_addr_start) begin
                apple_write_reserved_q <= 1'b1;
            end
            if (apple_io_write_start) begin
                apple_write_reserved_q <= 1'b0;
            end
            if (apple_io_read_start && (eth_state_q != ETH_IDLE) &&
                !apple_read_pending_q) begin
                apple_read_pending_q <= 1'b1;
                apple_read_pending_reg_q <= apple_io_reg;
                read_pending_q <= 1'b1;
                read_valid_q <= 1'b0;
            end

            if (host_req && !host_can_start && !host_pending_q) begin
                host_pending_q <= 1'b1;
                host_pending_write_q <= host_write;
                host_pending_addr_q <= host_addr & 16'h7FFF;
                host_pending_wdata_q <= host_wdata;
            end

            if (!reset_done_q) begin
                if (!reset_released_q) begin
                    eth_rst_n <= 1'b0;
                    if (reset_count_q == RESET_HOLD_COUNT) begin
                        reset_released_q <= 1'b1;
                        reset_count_q <= '0;
                        eth_rst_n <= 1'b1;
                    end else begin
                        reset_count_q <= reset_count_q + 1'b1;
                    end
                end else begin
                    eth_rst_n <= 1'b1;
                    if (reset_count_q == RESET_READY_WAIT_COUNT) begin
                        reset_count_q <= '0;
                        reset_done_q <= 1'b1;
                        addr_synced_q <= 1'b0;
                    end else begin
                        reset_count_q <= reset_count_q + 1'b1;
                    end
                end
            end else begin
                eth_rst_n <= 1'b1;
            end

            if (ab_read.data_en) begin
                read_pending_q <= 1'b0;
                read_valid_q <= 1'b0;
            end

            if (host_active_q && apple_waiting &&
                (eth_state_q != ETH_BUS_OP) && (eth_state_q != ETH_RECOVER)) begin
                fail_host_access();
            end else case (eth_state_q)
                ETH_IDLE: begin
                    eth_d_oe <= 1'b0;
                    eth_rd_n <= 1'b1;
                    eth_wr_n <= 1'b1;
                    eth_cs_n <= 1'b1;
                    bus_kind_q <= BUS_KIND_NONE;
                    op_cycle_q <= '0;

                    if (reset_done_q && apple_read_pending_q) begin
                        apple_read_pending_q <= 1'b0;
                        start_apple_read(apple_read_pending_reg_q);
                    end else if (reset_done_q && apple_io_read_start) begin
                        start_apple_read(apple_io_reg);
                    end else if (reset_done_q && apple_io_write_start) begin
                        case (apple_io_reg)
                            U2_REG_MODE: begin
                                mode_q <= next_mode_value(ab_read.data);
                                addr_synced_q <= 1'b0;
                                begin_bus_op(
                                    U2_REG_MODE,
                                    1'b1,
                                    w5100s_physical_mode_value(ab_read.data),
                                    BUS_KIND_MODE_WRITE,
                                    ETH_IDLE
                                );
                            end

                            U2_REG_ARH: begin
                                data_addr_q <= arh_shadow_addr;
                                access_addr_q <= arh_phys_addr;
                                begin_bus_op(
                                    U2_REG_ARH,
                                    1'b1,
                                    arh_phys_addr[15:8],
                                    BUS_KIND_ADDR_HI,
                                    ETH_IDLE
                                );
                            end

                            U2_REG_ARL: begin
                                data_addr_q <= arl_shadow_addr;
                                access_addr_q <= arl_phys_addr;
                                begin_bus_op(
                                    U2_REG_ARL,
                                    1'b1,
                                    arl_phys_addr[7:0],
                                    BUS_KIND_ADDR_LO,
                                    ETH_IDLE
                                );
                            end

                            U2_REG_DATA: begin
                                if (data_phys_addr == 16'h0000) begin
                                    mode_q <= next_mode_value(ab_read.data);
                                    if (next_mode_value(ab_read.data) & W5100_MR_AI) begin
                                        data_addr_q <= data_shadow_next_addr;
                                    end
                                    addr_synced_q <= 1'b0;
                                    begin_bus_op(
                                        U2_REG_MODE,
                                        1'b1,
                                        w5100s_physical_mode_value(ab_read.data),
                                        BUS_KIND_MODE_WRITE,
                                        ETH_IDLE
                                    );
                                end else if (w5100_reserved_addr(data_phys_addr)) begin
                                    mark_shadow_data_access();
                                end else begin
                                    access_addr_q <= data_phys_addr;
                                    payload_data_q <= ab_read.data;
                                    if (addr_synced_q && (phys_addr_q == data_phys_addr)) begin
                                        start_data_write(ab_read.data);
                                    end else begin
                                        start_addr_reload(data_phys_addr, ETH_ADDR_LO_THEN_WRITE);
                                    end
                                end
                            end

                            default: begin
                            end
                        endcase
                    end else if (reset_done_q && host_pending_q &&
                                 !apple_write_reserved_q &&
                                 !apple_io_write_addr_start) begin
                        host_pending_q <= 1'b0;
                        host_active_q <= 1'b1;
                        access_addr_q <= host_pending_addr_q;
                        payload_data_q <= host_pending_wdata_q;
                        start_host_mode_setup(
                            host_pending_write_q ? ETH_HOST_ADDR_RELOAD_WRITE :
                                                   ETH_HOST_ADDR_RELOAD_READ
                        );
                    end else if (host_can_start && host_req) begin
                        host_active_q <= 1'b1;
                        access_addr_q <= host_phys_addr;
                        payload_data_q <= host_wdata;
                        start_host_mode_setup(
                            host_write ? ETH_HOST_ADDR_RELOAD_WRITE :
                                         ETH_HOST_ADDR_RELOAD_READ
                        );
                    end
                end

                ETH_ADDR_LO_THEN_READ: begin
                    begin_bus_op(
                        U2_REG_ARL,
                        1'b1,
                        access_addr_q[7:0],
                        BUS_KIND_ADDR_LO,
                        ETH_DATA_READ_START
                    );
                end

                ETH_ADDR_LO_THEN_WRITE: begin
                    begin_bus_op(
                        U2_REG_ARL,
                        1'b1,
                        access_addr_q[7:0],
                        BUS_KIND_ADDR_LO,
                        ETH_DATA_WRITE_START
                    );
                end

                ETH_DATA_READ_START: begin
                    start_data_read();
                end

                ETH_DATA_WRITE_START: begin
                    start_data_write(payload_data_q);
                end

                ETH_HOST_ADDR_RELOAD_READ: begin
                    start_addr_reload(access_addr_q, ETH_HOST_ADDR_LO_THEN_READ);
                end

                ETH_HOST_ADDR_RELOAD_WRITE: begin
                    start_addr_reload(access_addr_q, ETH_HOST_ADDR_LO_THEN_WRITE);
                end

                ETH_HOST_ADDR_LO_THEN_READ: begin
                    begin_bus_op(
                        U2_REG_ARL,
                        1'b1,
                        access_addr_q[7:0],
                        BUS_KIND_ADDR_LO,
                        ETH_HOST_DATA_READ_START
                    );
                end

                ETH_HOST_ADDR_LO_THEN_WRITE: begin
                    begin_bus_op(
                        U2_REG_ARL,
                        1'b1,
                        access_addr_q[7:0],
                        BUS_KIND_ADDR_LO,
                        ETH_HOST_DATA_WRITE_START
                    );
                end

                ETH_HOST_DATA_READ_START: begin
                    start_host_read();
                end

                ETH_HOST_DATA_WRITE_START: begin
                    start_host_write(payload_data_q);
                end

                ETH_BUS_OP: begin
                    eth_a <= bus_reg_q;
                    eth_d_o <= access_data_q;
                    eth_d_oe <= bus_write_q;
                    eth_rd_n <= 1'b1;
                    eth_wr_n <= 1'b1;

                    if (op_cycle_q < BUS_SETUP_COUNT) begin
                        eth_cs_n <= 1'b1;
                        op_cycle_q <= op_cycle_q + 1'b1;
                    end else if (op_cycle_q ==
                                 (bus_write_q ? WRITE_DONE_COUNT : READ_DONE_COUNT)) begin
                        eth_cs_n <= 1'b1;
                        eth_rd_n <= 1'b1;
                        eth_wr_n <= 1'b1;
                        eth_d_oe <= 1'b0;
                        op_cycle_q <= '0;

                        case (bus_kind_q)
                            BUS_KIND_ADDR_HI: begin
                                phys_addr_q <= {access_addr_q[15:8], phys_addr_q[7:0]};
                                addr_synced_q <=
                                    ({access_addr_q[15:8], phys_addr_q[7:0]} == access_addr_q);
                            end

                            BUS_KIND_ADDR_LO: begin
                                phys_addr_q <= {phys_addr_q[15:8], access_addr_q[7:0]};
                                addr_synced_q <=
                                    ({phys_addr_q[15:8], access_addr_q[7:0]} == access_addr_q);
                            end

                            BUS_KIND_DATA_READ: begin
                                read_data_q <= eth_d_i;
                                read_valid_q <= 1'b1;
                                mark_physical_data_access();
                            end

                            BUS_KIND_DATA_WRITE: begin
                                mark_physical_data_access();
                            end

                            BUS_KIND_MODE_WRITE: begin
                                addr_synced_q <= 1'b0;
                            end

                            BUS_KIND_HOST_READ: begin
                                host_rdata_q <= eth_d_i;
                                host_done_q <= 1'b1;
                                host_active_q <= 1'b0;
                                phys_addr_q <= access_addr_q + 16'h0001;
                                addr_synced_q <= 1'b0;
                            end

                            BUS_KIND_HOST_WRITE: begin
                                host_done_q <= 1'b1;
                                host_active_q <= 1'b0;
                                phys_addr_q <= access_addr_q + 16'h0001;
                                addr_synced_q <= 1'b0;
                            end

                            default: begin
                            end
                        endcase

                        eth_state_q <= ETH_RECOVER;
                    end else begin
                        eth_cs_n <= 1'b0;
                        if (bus_write_q) begin
                            eth_wr_n <= 1'b0;
                        end else begin
                            eth_rd_n <= 1'b0;
                        end
                        op_cycle_q <= op_cycle_q + 1'b1;
                    end
                end

                ETH_RECOVER: begin
                    eth_d_oe <= 1'b0;
                    eth_rd_n <= 1'b1;
                    eth_wr_n <= 1'b1;
                    eth_cs_n <= 1'b1;
                    if (op_cycle_q == RECOVER_HOLD_COUNT) begin
                        op_cycle_q <= '0;
                        if (host_active_q && apple_waiting &&
                            (next_state_q != ETH_IDLE)) begin
                            fail_host_access();
                        end else begin
                            eth_state_q <= next_state_q;
                        end
                    end else begin
                        op_cycle_q <= op_cycle_q + 1'b1;
                    end
                end

                default: begin
                    eth_state_q <= ETH_IDLE;
                end
            endcase
        end
    end

    wire _unused_eth_int_n = eth_int_n;

endmodule
