`timescale 1ns / 1ps

// AppleMouse-compatible slot front end.
//
// The PS publishes USB HID mouse state through AxiSimple. The Apple side sees:
//   - a slot ROM at $Cn00-$CnFF with the AppleMouse ID bytes and entry table
//   - 16 I/O bytes at $C0n0-$C0nF used only by that ROM
//
// This deliberately keeps the firmware off $Cn80-style device addresses.
module mouse_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  logic                     vblank_start_pulse,
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output globals::AppleBus_write   ab_write,
    // Debug taps (freeze diagnosis): the mode register the app programmed
    // (enable + which IRQ sources), and the pending latches.
    output logic [3:0]               dbg_mode,
    output logic                     dbg_vbl_pending,
    output logic                     dbg_irq_pending
);

    localparam logic [7:0] AXI_REG_STATUS  = 8'h00;
    localparam logic [7:0] AXI_REG_X       = 8'h01;
    localparam logic [7:0] AXI_REG_Y       = 8'h02;
    localparam logic [7:0] AXI_REG_BUTTONS = 8'h03;
    localparam logic [7:0] AXI_REG_COMMIT  = 8'h04;
    localparam logic [7:0] AXI_REG_MODE    = 8'h05;

    localparam logic [3:0] IO_STATUS       = 4'h0;
    localparam logic [3:0] IO_X_LO         = 4'h1;
    localparam logic [3:0] IO_X_HI         = 4'h2;
    localparam logic [3:0] IO_Y_LO         = 4'h3;
    localparam logic [3:0] IO_Y_HI         = 4'h4;
    localparam logic [3:0] IO_BUTTONS      = 4'h5;
    localparam logic [3:0] IO_SEQ          = 4'h6;
    localparam logic [3:0] IO_CLAMP_AXIS   = 4'h7;
    localparam logic [3:0] IO_CLAMP_MIN_LO = 4'h8;
    localparam logic [3:0] IO_CLAMP_MIN_HI = 4'h9;
    localparam logic [3:0] IO_CLAMP_MAX_LO = 4'hA;
    localparam logic [3:0] IO_CLAMP_MAX_HI = 4'hB;
    localparam logic [3:0] IO_COMMAND      = 4'hC;
    localparam logic [3:0] IO_MODE         = 4'hE;
    localparam logic [3:0] IO_ACK          = 4'hF;

    localparam logic [7:0] CMD_HOME           = 8'h01;
    localparam logic [7:0] CMD_CLAMP_CURRENT  = 8'h02;
    localparam logic [7:0] CMD_CLAMP_DEFAULTS = 8'h03;  // INITMOUSE: 0..1023 both axes

    localparam int unsigned MODE_ENABLE_BIT     = 0;
    localparam int unsigned MODE_MOVE_IRQ_BIT   = 1;
    localparam int unsigned MODE_BUTTON_IRQ_BIT = 2;
    localparam int unsigned MODE_VBL_IRQ_BIT    = 3;

    function automatic logic [15:0] clamp16(
        input logic [15:0] value,
        input logic [15:0] min_value,
        input logic [15:0] max_value
    );
        logic [15:0] effective_max;
        begin
            effective_max = (max_value < min_value) ? min_value : max_value;
            if (value < min_value)
                clamp16 = min_value;
            else if (value > effective_max)
                clamp16 = effective_max;
            else
                clamp16 = value;
        end
    endfunction

    /* AppleWin-compatible clamp normalization (Blazing Paddles class): a
     * min > max window becomes 0 .. (min+max) & $FFFF. Applied when the
     * firmware commits a clamp with CMD_CLAMP_CURRENT; idempotent for
     * already-valid windows. Returns {min, max}. */
    function automatic logic [31:0] clamp_norm(
        input logic [15:0] min_value,
        input logic [15:0] max_value
    );
        begin
            if (min_value > max_value)
                clamp_norm = {16'd0, min_value + max_value};
            else
                clamp_norm = {min_value, max_value};
        end
    endfunction

    logic [7:0] slot_rom [0:255];
    initial begin
        $readmemh("mouse_card_slot2.mem", slot_rom);
    end

    /* These registers have several independent update sources. Letting
     * synthesis extract a shared clock enable folds the AXI/Apple decode and
     * clamp comparisons into one high-fanout CE path. Keep the enables local
     * in the D muxes instead. */
    (* extract_enable = "no" *) logic [15:0] mouse_x_q;
    (* extract_enable = "no" *) logic [15:0] mouse_y_q;
    logic [7:0]  buttons_q;
    logic [7:0]  prev_buttons_q;
    logic [7:0]  seq_q;
    logic        connected_q;
    /* The real card distinguishes "position changed since READMOUSE"
     * (status bit 5) from "movement requested an interrupt" (bit 1).
     * In particular, VBL-only mode $09 must retain bit 5 without inventing
     * a movement interrupt reason. */
    logic        movement_since_read_q;
    logic        movement_irq_pending_q;
    logic        button_pending_q;
    logic        vbl_pending_q;
    logic        irq_pending_q;
    logic [3:0]  mode_q;
    assign dbg_mode        = mode_q;
    assign dbg_vbl_pending = vbl_pending_q;
    assign dbg_irq_pending = irq_pending_q;
    logic        clamp_axis_q;
    logic [15:0] clamp_x_min_q;
    logic [15:0] clamp_x_max_q;
    logic [15:0] clamp_y_min_q;
    logic [15:0] clamp_y_max_q;
    /* Normalized views used at CMD_CLAMP_CURRENT commit time. */
    wire [31:0] clamp_x_norm = clamp_norm(clamp_x_min_q, clamp_x_max_q);
    wire [31:0] clamp_y_norm = clamp_norm(clamp_y_min_q, clamp_y_max_q);

    /* Clamp commands are completed on the following fabric clock. This
     * separates window normalization (compare/add) from position clamping
     * (two comparisons), removing their former five-CARRY4 cascade. The
     * command-time position and both normalized windows are captured so the
     * result is identical to the original single-edge implementation. One
     * 133 MHz tick is well inside a single Apple/vTW bus transaction. */
    logic        clamp_apply_pending_q;
    logic [15:0] clamp_apply_x_q;
    logic [15:0] clamp_apply_y_q;
    logic [15:0] clamp_apply_x_min_q;
    logic [15:0] clamp_apply_x_max_q;
    logic [15:0] clamp_apply_y_min_q;
    logic [15:0] clamp_apply_y_max_q;

    logic [15:0] ps_x_shadow_q;
    logic [15:0] ps_y_shadow_q;
    logic [7:0] ps_buttons_shadow_q;
    logic       ps_connected_shadow_q;

    wire enabled = (slot_assign != 3'h0);
    wire apple_bus_active = enabled &&
                            ((slot_assign != 3'h3) || sss.sw_slotc3rom) &&
                            ab_read.res;
    wire mouse_enabled = mode_q[MODE_ENABLE_BIT];
    wire ps_position_changed =
        (ps_x_shadow_q != mouse_x_q) || (ps_y_shadow_q != mouse_y_q);
    wire slot_rom_hit =
        apple_bus_active &&
        sss.slot_access &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] == slot_assign);

    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));

    wire ab_rom_read = ab_read.serve_en && ab_read.rw && slot_rom_hit;
    wire ab_io_read  = ab_read.serve_en && ab_read.rw && slot_io_hit;
    wire ab_io_write = ab_read.data_en && !ab_read.rw && slot_io_hit;
    wire [3:0] io_idx = ab_read.addr[3:0];

    wire [7:0] mouse_status_byte = {
        buttons_q[0],
        prev_buttons_q[0],
        movement_since_read_q,
        buttons_q[1],
        vbl_pending_q,
        button_pending_q,
        movement_irq_pending_q,
        prev_buttons_q[1]
    };

    logic [7:0] io_read_byte;
    always_comb begin
        case (io_idx)
            IO_STATUS:  io_read_byte = mouse_status_byte;
            IO_X_LO:    io_read_byte = mouse_x_q[7:0];
            IO_X_HI:    io_read_byte = mouse_x_q[15:8];
            IO_Y_LO:    io_read_byte = mouse_y_q[7:0];
            IO_Y_HI:    io_read_byte = mouse_y_q[15:8];
            IO_BUTTONS: io_read_byte = buttons_q;
            IO_SEQ:     io_read_byte = seq_q;
            IO_CLAMP_AXIS:   io_read_byte = {7'b0, clamp_axis_q};
            IO_CLAMP_MIN_LO: io_read_byte = clamp_axis_q ? clamp_y_min_q[7:0]  : clamp_x_min_q[7:0];
            IO_CLAMP_MIN_HI: io_read_byte = clamp_axis_q ? clamp_y_min_q[15:8] : clamp_x_min_q[15:8];
            IO_CLAMP_MAX_LO: io_read_byte = clamp_axis_q ? clamp_y_max_q[7:0]  : clamp_x_max_q[7:0];
            IO_CLAMP_MAX_HI: io_read_byte = clamp_axis_q ? clamp_y_max_q[15:8] : clamp_x_max_q[15:8];
            IO_MODE:    io_read_byte = {4'b0, mode_q};
            default:    io_read_byte = 8'h00;
        endcase
    end

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        /* VBL interrupts are legal with the mouse itself disabled (mode
         * $08), so the enable bit gates only the pointer-driven sources. */
        /* IRQ output is a separate latch from the status byte. SERVEMOUSE
         * (ACK bit 1) releases only this latch; READMOUSE (ACK bit 0)
         * consumes the status causes and movement/button state. Keeping
         * those operations independent preserves a new IRQ which arrives
         * between the two calls, matching the original AppleMouse. */
        ab_write_d.assert_irq = apple_bus_active && irq_pending_q;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (ab_read.serve_en) begin
            if (ab_rom_read) begin
                ab_write_d.wr_data = slot_rom[ab_read.addr[7:0]];
                ab_write_d.wr_data_en = 1'b1;
            end else if (ab_io_read) begin
                ab_write_d.wr_data = io_read_byte;
                ab_write_d.wr_data_en = 1'b1;
            end else begin
                ab_write_d.wr_data = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
            end
        end else if (ab_read.data_en) begin
            ab_write_d.wr_data = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    logic [7:0] axi_read_addr_q;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            mouse_x_q <= 16'd0;
            mouse_y_q <= 16'd0;
            buttons_q <= 8'd0;
            prev_buttons_q <= 8'd0;
            seq_q <= 8'd0;
            connected_q <= 1'b0;
            movement_since_read_q <= 1'b0;
            movement_irq_pending_q <= 1'b0;
            button_pending_q <= 1'b0;
            vbl_pending_q <= 1'b0;
            irq_pending_q <= 1'b0;
            mode_q <= 4'd0;
            clamp_axis_q <= 1'b0;
            clamp_x_min_q <= 16'd0;
            clamp_x_max_q <= 16'd1023;
            clamp_y_min_q <= 16'd0;
            clamp_y_max_q <= 16'd1023;
            clamp_apply_pending_q <= 1'b0;
            clamp_apply_x_q <= 16'd0;
            clamp_apply_y_q <= 16'd0;
            clamp_apply_x_min_q <= 16'd0;
            clamp_apply_x_max_q <= 16'd1023;
            clamp_apply_y_min_q <= 16'd0;
            clamp_apply_y_max_q <= 16'd1023;
            ps_x_shadow_q <= 16'd0;
            ps_y_shadow_q <= 16'd0;
            ps_buttons_shadow_q <= 8'd0;
            ps_connected_shadow_q <= 1'b0;
            ab_write_q <= '0;
            axi_read_addr_q <= 8'd0;
        end else begin
            ab_write_q <= ab_write_d;
            axi_read_addr_q <= as_common.araddr;

            if (clamp_apply_pending_q) begin
                mouse_x_q <= clamp16(clamp_apply_x_q,
                                     clamp_apply_x_min_q,
                                     clamp_apply_x_max_q);
                mouse_y_q <= clamp16(clamp_apply_y_q,
                                     clamp_apply_y_min_q,
                                     clamp_apply_y_max_q);
                clamp_apply_pending_q <= 1'b0;
            end

            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    AXI_REG_STATUS: begin
                        if (as_common.wstrb[0])
                            ps_connected_shadow_q <= as_common.wdata[0];
                    end
                    AXI_REG_X: begin
                        if (as_common.wstrb[0] || as_common.wstrb[1])
                            ps_x_shadow_q <= as_common.wdata[15:0];
                    end
                    AXI_REG_Y: begin
                        if (as_common.wstrb[0] || as_common.wstrb[1])
                            ps_y_shadow_q <= as_common.wdata[15:0];
                    end
                    AXI_REG_BUTTONS: begin
                        if (as_common.wstrb[0])
                            ps_buttons_shadow_q <= as_common.wdata[7:0];
                    end
                    AXI_REG_COMMIT: begin
                        if (as_common.wstrb[0]) begin
                            connected_q <= ps_connected_shadow_q;
                            if (ps_connected_shadow_q && mouse_enabled) begin
                                if (ps_position_changed) begin
                                    movement_since_read_q <= 1'b1;
                                    if (mode_q[MODE_MOVE_IRQ_BIT]) begin
                                        movement_irq_pending_q <= 1'b1;
                                        irq_pending_q <= 1'b1;
                                    end
                                end
                                /* Bit 2 is an interrupt CAUSE bit: it
                                 * latches only when button interrupts are
                                 * armed, like the other cause bits. Button
                                 * STATE lives in bits 7/6/4/0. */
                                if (ps_buttons_shadow_q != buttons_q &&
                                    mode_q[MODE_BUTTON_IRQ_BIT]) begin
                                    button_pending_q <= 1'b1;
                                    irq_pending_q <= 1'b1;
                                end
                                mouse_x_q <= clamp16(ps_x_shadow_q, clamp_x_min_q, clamp_x_max_q);
                                mouse_y_q <= clamp16(ps_y_shadow_q, clamp_y_min_q, clamp_y_max_q);
                                buttons_q <= ps_buttons_shadow_q;
                            end else if (!ps_connected_shadow_q) begin
                                buttons_q <= 8'd0;
                            end
                            seq_q <= as_common.wdata[7:0];
                        end
                    end
                    default: begin
                    end
                endcase
            end

            if (ab_io_write) begin
                case (io_idx)
                    /* Absolute position writes store RAW values: the manual
                     * requires CLEARMOUSE to set the position to zero even
                     * with a nonzero clamp minimum, and AppleWin's
                     * SetPositionAbs does not clamp either. Clamping happens
                     * on PS delta commits, CMD_HOME, and clamp commits. */
                    IO_X_LO: mouse_x_q[7:0] <= ab_read.data;
                    IO_X_HI: mouse_x_q[15:8] <= ab_read.data;
                    IO_Y_LO: mouse_y_q[7:0] <= ab_read.data;
                    IO_Y_HI: mouse_y_q[15:8] <= ab_read.data;
                    IO_CLAMP_AXIS: clamp_axis_q <= ab_read.data[0];
                    IO_CLAMP_MIN_LO: begin
                        if (clamp_axis_q)
                            clamp_y_min_q[7:0] <= ab_read.data;
                        else
                            clamp_x_min_q[7:0] <= ab_read.data;
                    end
                    IO_CLAMP_MIN_HI: begin
                        if (clamp_axis_q)
                            clamp_y_min_q[15:8] <= ab_read.data;
                        else
                            clamp_x_min_q[15:8] <= ab_read.data;
                    end
                    IO_CLAMP_MAX_LO: begin
                        if (clamp_axis_q)
                            clamp_y_max_q[7:0] <= ab_read.data;
                        else
                            clamp_x_max_q[7:0] <= ab_read.data;
                    end
                    IO_CLAMP_MAX_HI: begin
                        if (clamp_axis_q)
                            clamp_y_max_q[15:8] <= ab_read.data;
                        else
                            clamp_x_max_q[15:8] <= ab_read.data;
                    end
                    IO_COMMAND: begin
                        if (ab_read.data == CMD_HOME) begin
                            mouse_x_q <= clamp_x_min_q;
                            mouse_y_q <= clamp_y_min_q;
                            clamp_apply_pending_q <= 1'b0;
                        end else if (ab_read.data == CMD_CLAMP_CURRENT) begin
                            /* Commit the freshly written window: normalize
                             * min > max AppleWin-style. Capture both windows
                             * and the current position; the short clamp stage
                             * completes on the following fabric clock. */
                            if (clamp_axis_q) begin
                                clamp_y_min_q <= clamp_y_norm[31:16];
                                clamp_y_max_q <= clamp_y_norm[15:0];
                            end else begin
                                clamp_x_min_q <= clamp_x_norm[31:16];
                                clamp_x_max_q <= clamp_x_norm[15:0];
                            end
                            clamp_apply_pending_q <= 1'b1;
                            clamp_apply_x_q <= mouse_x_q;
                            clamp_apply_y_q <= mouse_y_q;
                            clamp_apply_x_min_q <= clamp_x_norm[31:16];
                            clamp_apply_x_max_q <= clamp_x_norm[15:0];
                            clamp_apply_y_min_q <= clamp_y_norm[31:16];
                            clamp_apply_y_max_q <= clamp_y_norm[15:0];
                        end else if (ab_read.data == CMD_CLAMP_DEFAULTS) begin
                            /* INITMOUSE: both windows back to 0..1023. */
                            clamp_x_min_q <= 16'd0;
                            clamp_x_max_q <= 16'd1023;
                            clamp_y_min_q <= 16'd0;
                            clamp_y_max_q <= 16'd1023;
                            clamp_apply_pending_q <= 1'b1;
                            clamp_apply_x_q <= mouse_x_q;
                            clamp_apply_y_q <= mouse_y_q;
                            clamp_apply_x_min_q <= 16'd0;
                            clamp_apply_x_max_q <= 16'd1023;
                            clamp_apply_y_min_q <= 16'd0;
                            clamp_apply_y_max_q <= 16'd1023;
                        end
                    end
                    IO_MODE: begin
                        mode_q <= ab_read.data[3:0];
                        /* SETMOUSE changes only the mode. Pending status and
                         * IRQ state remain owned by READMOUSE/SERVEMOUSE. */
                    end
                    IO_ACK: begin
                        /* The real card's SERVE command deasserts IRQ but
                         * leaves its cause byte intact for READMOUSE.
                         * READMOUSE then consumes all cause bits and the
                         * movement-since-read state without touching the
                         * IRQ output latch. If a new event arrives between
                         * SERVE and READ, READ clears its cause but the new
                         * IRQ remains asserted; the next SERVE releases it
                         * and correctly reports a reasonless interrupt. */
                        if (ab_read.data[0]) begin
                            movement_since_read_q <= 1'b0;
                            movement_irq_pending_q <= 1'b0;
                            button_pending_q <= 1'b0;
                            vbl_pending_q <= 1'b0;
                            prev_buttons_q <= buttons_q;
                        end
                        if (ab_read.data[1]) begin
                            irq_pending_q <= 1'b0;
                        end
                    end
                    default: begin
                    end
                endcase
            end

            /* VBL interrupts do not require the mouse enable bit: the real
             * AppleMouse supports mode $08 (VBL only, mouse off). */
            if (vblank_start_pulse && mode_q[MODE_VBL_IRQ_BIT]) begin
                vbl_pending_q <= 1'b1;
                irq_pending_q <= 1'b1;
            end

            if (!ab_read.res || !enabled) begin
                movement_since_read_q <= 1'b0;
                movement_irq_pending_q <= 1'b0;
                button_pending_q <= 1'b0;
                vbl_pending_q <= 1'b0;
                irq_pending_q <= 1'b0;
                mode_q <= 4'd0;
                clamp_axis_q <= 1'b0;
                clamp_x_min_q <= 16'd0;
                clamp_x_max_q <= 16'd1023;
                clamp_y_min_q <= 16'd0;
                clamp_y_max_q <= 16'd1023;
                clamp_apply_pending_q <= 1'b0;
                ab_write_q <= '0;
            end
        end
    end

    always_comb begin
        case (axi_read_addr_q)
            AXI_REG_STATUS: as_client.rdata = {
                20'd0,
                irq_pending_q,
                button_pending_q,
                movement_irq_pending_q,
                connected_q,
                buttons_q
            };
            AXI_REG_X:       as_client.rdata = {16'd0, mouse_x_q};
            AXI_REG_Y:       as_client.rdata = {16'd0, mouse_y_q};
            AXI_REG_BUTTONS: as_client.rdata = {24'd0, buttons_q};
            AXI_REG_COMMIT:  as_client.rdata = {24'd0, seq_q};
            AXI_REG_MODE:    as_client.rdata = {28'd0, mode_q};
            8'h06:           as_client.rdata = {16'd0, clamp_x_min_q};
            8'h07:           as_client.rdata = {16'd0, clamp_x_max_q};
            8'h08:           as_client.rdata = {16'd0, clamp_y_min_q};
            8'h09:           as_client.rdata = {16'd0, clamp_y_max_q};
            default:         as_client.rdata = 32'h0000_0000;
        endcase
    end

endmodule
