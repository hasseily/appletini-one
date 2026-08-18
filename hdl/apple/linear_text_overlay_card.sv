`timescale 1ns / 1ps

// Linear RAM text overlay, version 1.0.
//
// The Apple-visible register block follows
// docs/linear_ram_text_overlay_interface_v1.0.html. The block shares the
// SmartPort slot and therefore sees an already-gated Apple bus from
// smartport_card: SuperSprite and the boot menu can take slot 7 without a
// second ownership path.
module linear_text_overlay_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic [2:0]               slot_assign,
    input  logic                     capture_drop_sticky,
    input  logic                     canvas_shr_active,

    input  logic                     ps_wr_en,
    input  logic [7:0]               ps_addr,
    input  logic [31:0]              ps_wdata,
    input  logic [7:0]               ps_read_addr,
    output logic [31:0]              ps_rdata,

    output logic                     io_read_valid,
    output logic [7:0]               io_read_data,
    output logic                     devsel_enabled,
    output logic                     capture_armed,
    output logic                     capture_bank_aux,
    output logic [15:0]              capture_base,
    output logic [15:0]              capture_limit
);

    localparam logic [7:0] REG_STATE   = 8'h20;
    localparam logic [7:0] REG_ARM0    = 8'h21;
    localparam logic [7:0] REG_ARM1    = 8'h22;
    localparam logic [7:0] REG_ARM2    = 8'h23;
    localparam logic [7:0] REG_CURSOR  = 8'h24;
    localparam logic [7:0] REG_ACTIVE0 = 8'h25;
    localparam logic [7:0] REG_ACTIVE1 = 8'h26;
    localparam logic [7:0] REG_ACTIVE2 = 8'h27;
    localparam logic [7:0] REG_CONTROL = 8'h28;

    localparam logic [1:0] FRAME_NONE = 2'd0;
    localparam logic [1:0] FRAME_SHOW = 2'd1;
    localparam logic [1:0] FRAME_HIDE = 2'd2;
    localparam logic [1:0] FRAME_OFF  = 2'd3;

    localparam logic [7:0] CMD_OFF  = 8'h00;
    localparam logic [7:0] CMD_ARM  = 8'h01;
    localparam logic [7:0] CMD_SHOW = 8'h02;
    localparam logic [7:0] CMD_HIDE = 8'h03;

    localparam logic [15:0] LEGACY_CANVAS_WIDTH  = 16'd1120;
    localparam logic [15:0] LEGACY_CANVAS_HEIGHT = 16'd768;
    localparam logic [15:0] SHR_CANVAS_WIDTH     = 16'd1280;
    localparam logic [15:0] SHR_CANVAS_HEIGHT    = 16'd800;

    wire [15:0] canvas_width = canvas_shr_active ?
        SHR_CANVAS_WIDTH : LEGACY_CANVAS_WIDTH;
    wire [15:0] canvas_height = canvas_shr_active ?
        SHR_CANVAS_HEIGHT : LEGACY_CANVAS_HEIGHT;

    logic [7:0] index_q;

    logic [15:0] staged_base_q;
    logic [7:0]  staged_config_q;
    logic [7:0]  staged_cols_q;
    logic [7:0]  staged_rows_q;
    logic [15:0] staged_origin_x_q;
    logic [15:0] staged_origin_y_q;
    logic [7:0]  staged_scale_q;
    logic [7:0]  fill_char_q;
    logic [7:0]  fill_attr_q;

    logic [7:0]  cursor_x_q;
    logic [7:0]  cursor_y_q;
    logic [7:0]  cursor_ctrl_q;

    logic [15:0] armed_base_q;
    logic [15:0] armed_limit_q;
    logic [7:0]  armed_config_q;
    logic [7:0]  armed_cols_q;
    logic [7:0]  armed_rows_q;
    logic [15:0] armed_origin_x_q;
    logic [15:0] armed_origin_y_q;
    logic [7:0]  armed_scale_q;
    logic [7:0]  armed_fill_char_q;
    logic [7:0]  armed_fill_attr_q;
    logic        armed_slot_q;

    logic [15:0] active_base_q;
    logic [7:0]  active_config_q;
    logic [7:0]  active_cols_q;
    logic [7:0]  active_rows_q;
    logic [15:0] active_origin_x_q;
    logic [15:0] active_origin_y_q;
    logic [7:0]  active_scale_q;
    logic        active_slot_q;

    logic busy_q;
    logic stale_q;
    logic config_error_q;
    logic frame_pending_q;
    logic armed_q;
    logic visible_q;
    logic arm_request_q;
    logic arm_validate_q;
    logic [3:0] arm_mul_step_q;
    logic [15:0] arm_mul_multiplicand_q;
    logic [7:0] arm_mul_multiplier_q;
    logic [15:0] arm_mul_product_q;
    logic show_drained_q;
    logic capture_drop_seen_q;
    logic [1:0] frame_command_q;

    wire enabled = (slot_assign != 3'd0);
    wire apple_bus_active = enabled && ab_read.res &&
                            ((slot_assign != 3'd3) || sss.sw_slotc3rom);
    wire slot_io_hit = apple_bus_active &&
                       (ab_read.addr[15:8] == 8'hC0) &&
                       (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire ab_io_read  = ab_read.serve_en && ab_read.rw && slot_io_hit;
    wire ab_io_write = ab_read.data_en && !ab_read.rw && slot_io_hit;
    wire [3:0] io_idx = ab_read.addr[3:0];

    /* The parent gates both phase strobes when SmartPort does not own slot 7.
     * Export a cycle-qualified enable so the separate capture tap cannot
     * record a command written while SuperSprite or the boot menu owns it. */
    assign devsel_enabled  = apple_bus_active &&
                             (ab_read.serve_en || ab_read.data_en);
    assign capture_armed   = armed_q;
    assign capture_bank_aux = armed_config_q[0];
    assign capture_base    = armed_base_q;
    assign capture_limit   = armed_limit_q;

    wire staged_static_valid =
        (staged_cols_q != 8'd0) &&
        (staged_rows_q != 8'd0) &&
        !staged_rows_q[7] &&
        (staged_scale_q[3:0] != 4'd0) &&
        (staged_scale_q[7:4] != 4'd0) &&
        (staged_base_q >= 16'h0200) &&
        (staged_config_q[7:5] == 3'b000);
    wire [15:0] arm_mul_addend = arm_mul_multiplier_q[0]
        ? arm_mul_multiplicand_q : 16'd0;
    wire [15:0] arm_mul_product_next = arm_mul_product_q + arm_mul_addend;
    wire [16:0] arm_limit_next =
        {1'b0, staged_base_q} + {arm_mul_product_next, 1'b0};

    wire [7:0] status_byte = {
        busy_q,
        stale_q,
        config_error_q,
        frame_pending_q,
        2'b00,
        armed_q,
        visible_q
    };

    logic [7:0] indexed_read_data;
    always_comb begin
        case (index_q)
            8'h00: indexed_read_data = staged_base_q[7:0];
            8'h01: indexed_read_data = staged_base_q[15:8];
            8'h02: indexed_read_data = staged_config_q;
            8'h03: indexed_read_data = staged_cols_q;
            8'h04: indexed_read_data = staged_rows_q;
            8'h05: indexed_read_data = staged_origin_x_q[7:0];
            8'h06: indexed_read_data = staged_origin_x_q[15:8];
            8'h07: indexed_read_data = staged_origin_y_q[7:0];
            8'h08: indexed_read_data = staged_origin_y_q[15:8];
            8'h09: indexed_read_data = staged_scale_q;
            8'h0A: indexed_read_data = cursor_x_q;
            8'h0B: indexed_read_data = cursor_y_q;
            8'h0C: indexed_read_data = cursor_ctrl_q;
            8'h0D: indexed_read_data = fill_char_q;
            8'h0E: indexed_read_data = fill_attr_q;
            8'h10: indexed_read_data = canvas_width[7:0];
            8'h11: indexed_read_data = canvas_width[15:8];
            8'h12: indexed_read_data = canvas_height[7:0];
            8'h13: indexed_read_data = canvas_height[15:8];
            8'h14: indexed_read_data = active_base_q[7:0];
            8'h15: indexed_read_data = active_base_q[15:8];
            8'h16: indexed_read_data = active_config_q;
            8'h17: indexed_read_data = active_cols_q;
            8'h18: indexed_read_data = active_rows_q;
            8'h19: indexed_read_data = active_origin_x_q[7:0];
            8'h1A: indexed_read_data = active_origin_x_q[15:8];
            8'h1B: indexed_read_data = active_origin_y_q[7:0];
            8'h1C: indexed_read_data = active_origin_y_q[15:8];
            8'h1D: indexed_read_data = active_scale_q;
            8'h1E: indexed_read_data = 8'h7F;
            default: indexed_read_data = 8'h00;
        endcase
    end

    always_comb begin
        case (io_idx)
            4'h0: io_read_data = index_q;
            4'h1,
            4'h2: io_read_data = indexed_read_data;
            4'h4: io_read_data = status_byte;
            4'h8: io_read_data = "L";
            4'h9: io_read_data = "I";
            4'hA: io_read_data = "N";
            4'hB: io_read_data = "T";
            4'hC: io_read_data = "X";
            4'hD: io_read_data = "T";
            4'hE: io_read_data = 8'h4C;
            4'hF: io_read_data = 8'h10;
            default: io_read_data = 8'h00;
        endcase
    end
    assign io_read_valid = ab_io_read;

    task automatic reset_overlay_state;
        begin
            index_q             <= 8'h00;
            staged_base_q       <= 16'h0000;
            staged_config_q     <= 8'h08;
            staged_cols_q       <= 8'd80;
            staged_rows_q       <= 8'd24;
            staged_origin_x_q   <= 16'h0000;
            staged_origin_y_q   <= 16'h0000;
            staged_scale_q      <= 8'h11;
            fill_char_q         <= 8'h20;
            fill_attr_q         <= 8'h07;
            cursor_x_q          <= 8'h00;
            cursor_y_q          <= 8'h00;
            cursor_ctrl_q       <= 8'h00;
            armed_base_q        <= 16'h0000;
            armed_limit_q       <= 16'h0000;
            armed_config_q      <= 8'h08;
            armed_cols_q        <= 8'd80;
            armed_rows_q        <= 8'd24;
            armed_origin_x_q    <= 16'h0000;
            armed_origin_y_q    <= 16'h0000;
            armed_scale_q       <= 8'h11;
            armed_fill_char_q   <= 8'h20;
            armed_fill_attr_q   <= 8'h07;
            armed_slot_q        <= 1'b0;
            active_base_q       <= 16'h0000;
            active_config_q     <= 8'h08;
            active_cols_q       <= 8'd80;
            active_rows_q       <= 8'd24;
            active_origin_x_q   <= 16'h0000;
            active_origin_y_q   <= 16'h0000;
            active_scale_q      <= 8'h11;
            active_slot_q       <= 1'b0;
            busy_q              <= 1'b0;
            stale_q             <= 1'b0;
            config_error_q      <= 1'b0;
            frame_pending_q     <= 1'b0;
            armed_q             <= 1'b0;
            visible_q           <= 1'b0;
            arm_request_q       <= 1'b0;
            arm_validate_q      <= 1'b0;
            arm_mul_step_q      <= 4'd0;
            arm_mul_multiplicand_q <= 16'd0;
            arm_mul_multiplier_q <= 8'd0;
            arm_mul_product_q   <= 16'd0;
            show_drained_q      <= 1'b0;
            capture_drop_seen_q <= 1'b0;
            frame_command_q     <= FRAME_NONE;
        end
    endtask

    always_ff @(posedge clk) begin
        if (!rstn) begin
            reset_overlay_state();
        end else if (!ab_read.res) begin
            reset_overlay_state();
        end else begin
            if (!capture_drop_sticky)
                capture_drop_seen_q <= 1'b0;

            if (ab_io_write) begin
                case (io_idx)
                    4'h0: index_q <= ab_read.data;
                    4'h1,
                    4'h2: begin
                        case (index_q)
                            8'h00: staged_base_q[7:0]     <= ab_read.data;
                            8'h01: staged_base_q[15:8]    <= ab_read.data;
                            8'h02: staged_config_q        <= ab_read.data;
                            8'h03: staged_cols_q          <= ab_read.data;
                            8'h04: staged_rows_q          <= ab_read.data;
                            8'h05: staged_origin_x_q[7:0] <= ab_read.data;
                            8'h06: staged_origin_x_q[15:8] <= ab_read.data;
                            8'h07: staged_origin_y_q[7:0] <= ab_read.data;
                            8'h08: staged_origin_y_q[15:8] <= ab_read.data;
                            8'h09: staged_scale_q         <= ab_read.data;
                            8'h0A: cursor_x_q             <= ab_read.data;
                            8'h0B: cursor_y_q             <= {1'b0, ab_read.data[6:0]};
                            8'h0C: cursor_ctrl_q          <= {4'b0000, ab_read.data[3:0]};
                            8'h0D: fill_char_q            <= ab_read.data;
                            8'h0E: fill_attr_q            <= ab_read.data;
                            default: ;
                        endcase
                        if (io_idx == 4'h2)
                            index_q <= index_q + 8'd1;
                    end
                    4'h3: begin
                        if (!busy_q) begin
                            case (ab_read.data)
                                CMD_OFF: begin
                                    frame_pending_q <= 1'b1;
                                    frame_command_q <= FRAME_OFF;
                                    show_drained_q  <= 1'b1;
                                end
                                CMD_ARM: begin
                                    if (!staged_static_valid) begin
                                        config_error_q <= 1'b1;
                                    end else begin
                                        busy_q            <= 1'b1;
                                        arm_validate_q    <= 1'b1;
                                        arm_mul_step_q    <= 4'd0;
                                        arm_mul_multiplicand_q <=
                                            {8'd0, staged_cols_q};
                                        arm_mul_multiplier_q <= staged_rows_q;
                                        arm_mul_product_q <= 16'd0;
                                        if (capture_drop_sticky)
                                            capture_drop_seen_q <= 1'b1;
                                    end
                                end
                                CMD_SHOW: begin
                                    if (armed_q && !stale_q && !config_error_q) begin
                                        frame_pending_q <= 1'b1;
                                        frame_command_q <= FRAME_SHOW;
                                        show_drained_q  <= 1'b0;
                                    end
                                end
                                CMD_HIDE: begin
                                    frame_pending_q <= 1'b1;
                                    frame_command_q <= FRAME_HIDE;
                                    show_drained_q  <= 1'b1;
                                end
                                default: ;
                            endcase
                        end
                    end
                    default: ;
                endcase
            end

            if (ab_io_read && (io_idx == 4'h2))
                index_q <= index_q + 8'd1;

            /* ARM range multiplication runs over eight fabric clocks. This
             * keeps the 8x8 multiply and 17-bit limit add off the 133 MHz
             * Apple register path. BUSY was already set on the CMD cycle. */
            if (arm_validate_q) begin
                arm_mul_product_q <= arm_mul_product_next;
                arm_mul_multiplicand_q <= arm_mul_multiplicand_q << 1;
                arm_mul_multiplier_q <= arm_mul_multiplier_q >> 1;
                arm_mul_step_q <= arm_mul_step_q + 4'd1;
                if (arm_mul_step_q == 4'd7) begin
                    arm_validate_q <= 1'b0;
                    if (arm_limit_next <= 17'h0C000) begin
                        armed_base_q      <= staged_base_q;
                        armed_config_q    <= staged_config_q;
                        armed_cols_q      <= staged_cols_q;
                        armed_rows_q      <= staged_rows_q;
                        armed_origin_x_q  <= staged_origin_x_q;
                        armed_origin_y_q  <= staged_origin_y_q;
                        armed_scale_q     <= staged_scale_q;
                        armed_fill_char_q <= fill_char_q;
                        armed_fill_attr_q <= fill_attr_q;
                        armed_limit_q <= arm_limit_next[15:0];
                        armed_q       <= 1'b0;
                        stale_q       <= 1'b0;
                        config_error_q <= 1'b0;
                        arm_request_q <= 1'b1;
                    end else begin
                        busy_q         <= 1'b0;
                        config_error_q <= 1'b1;
                    end
                end
            end

            if (ps_wr_en && (ps_addr == REG_CONTROL)) begin
                if (ps_wdata[0] && arm_request_q) begin
                    armed_slot_q    <= ps_wdata[1];
                    arm_request_q   <= 1'b0;
                    busy_q          <= 1'b0;
                    armed_q         <= 1'b1;
                end
                if (ps_wdata[2] && frame_pending_q &&
                    (frame_command_q == FRAME_SHOW)) begin
                    show_drained_q <= 1'b1;
                end
                if (ps_wdata[3] && frame_pending_q &&
                    ((frame_command_q != FRAME_SHOW) || show_drained_q)) begin
                    case (frame_command_q)
                        FRAME_SHOW: begin
                            active_base_q     <= armed_base_q;
                            active_config_q   <= armed_config_q;
                            active_cols_q     <= armed_cols_q;
                            active_rows_q     <= armed_rows_q;
                            active_origin_x_q <= armed_origin_x_q;
                            active_origin_y_q <= armed_origin_y_q;
                            active_scale_q    <= armed_scale_q;
                            active_slot_q     <= armed_slot_q;
                            visible_q         <= 1'b1;
                        end
                        FRAME_HIDE: visible_q <= 1'b0;
                        FRAME_OFF: begin
                            visible_q <= 1'b0;
                            armed_q   <= 1'b0;
                        end
                        default: ;
                    endcase
                    frame_pending_q <= 1'b0;
                    frame_command_q <= FRAME_NONE;
                    show_drained_q  <= 1'b0;
                end
                if (ps_wdata[4] && armed_q) begin
                    stale_q         <= 1'b1;
                    armed_q         <= 1'b0;
                    frame_pending_q <= 1'b1;
                    frame_command_q <= FRAME_HIDE;
                    show_drained_q  <= 1'b1;
                end
            end

            if (capture_drop_sticky && !capture_drop_seen_q && armed_q &&
                !(ab_io_write && (io_idx == 4'h3) &&
                  (ab_read.data == CMD_ARM))) begin
                capture_drop_seen_q <= 1'b1;
                stale_q             <= 1'b1;
                armed_q             <= 1'b0;
                frame_pending_q     <= 1'b1;
                frame_command_q     <= FRAME_HIDE;
                show_drained_q      <= 1'b1;
            end
        end
    end

    always_comb begin
        case (ps_read_addr)
            REG_STATE: ps_rdata = {
                19'd0,
                visible_q,
                armed_q,
                frame_pending_q,
                config_error_q,
                stale_q,
                busy_q,
                active_slot_q,
                armed_slot_q,
                frame_command_q,
                show_drained_q,
                frame_pending_q,
                arm_request_q
            };
            REG_ARM0: ps_rdata = {
                armed_cols_q, armed_config_q, armed_base_q
            };
            REG_ARM1: ps_rdata = {
                armed_origin_y_q[7:0], armed_origin_x_q, armed_rows_q
            };
            REG_ARM2: ps_rdata = {
                armed_fill_attr_q, armed_fill_char_q,
                armed_scale_q, armed_origin_y_q[15:8]
            };
            REG_CURSOR: ps_rdata = {
                8'h00, cursor_ctrl_q, cursor_y_q, cursor_x_q
            };
            REG_ACTIVE0: ps_rdata = {
                active_cols_q, active_config_q, active_base_q
            };
            REG_ACTIVE1: ps_rdata = {
                active_origin_y_q[7:0], active_origin_x_q, active_rows_q
            };
            REG_ACTIVE2: ps_rdata = {
                7'h00, active_slot_q, 8'h00,
                active_scale_q, active_origin_y_q[15:8]
            };
            default: ps_rdata = 32'h00000000;
        endcase
    end

endmodule
