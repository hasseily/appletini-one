`timescale 1ns / 1ps

// Synthesizable, cycle-stepped WDC W65C02S-compatible processor core.
//
// The bus is presented as separate input/output signals so the core can be
// connected directly to FPGA block RAM or to a request/response bridge.  One
// bus cycle completes on each rising clk edge for which enable && ready is
// true.  Address, rwb, data_out, sync, vpb_n, and mlb_n remain stable while a
// cycle is stalled.
//
// DEBUG_STATE_LOAD adds an architectural-state load path for single-step
// vector simulation.  Set it to zero in hardware; synthesis then removes the
// debug muxes while retaining the read-only state outputs for diagnostics.
module w65c02_core #(
    parameter bit DEBUG_STATE_LOAD = 1'b0
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        enable,
    input  logic        ready,

    input  logic        irq_n,
    input  logic        nmi_n,
    input  logic        so_n,

    input  logic [7:0]  data_in,
    output logic [15:0] addr,
    output logic [7:0]  data_out,
    output logic        rwb,
    output logic        sync,
    output logic        vpb_n,
    output logic        mlb_n,
    output logic        waiting,
    output logic        stopped,
    output logic        instruction_done,

    input  logic        debug_load,
    input  logic [15:0] debug_pc_in,
    input  logic [7:0]  debug_s_in,
    input  logic [7:0]  debug_a_in,
    input  logic [7:0]  debug_x_in,
    input  logic [7:0]  debug_y_in,
    input  logic [7:0]  debug_p_in,
    output logic [15:0] debug_pc,
    output logic [7:0]  debug_s,
    output logic [7:0]  debug_a,
    output logic [7:0]  debug_x,
    output logic [7:0]  debug_y,
    output logic [7:0]  debug_p
);
    localparam int P_C = 0;
    localparam int P_Z = 1;
    localparam int P_I = 2;
    localparam int P_D = 3;
    localparam int P_V = 6;
    localparam int P_N = 7;

    typedef enum logic [6:0] {
        OP_NOP,
        OP_ORA, OP_AND, OP_EOR, OP_ADC, OP_SBC,
        OP_CMP, OP_CPX, OP_CPY, OP_BIT,
        OP_LDA, OP_LDX, OP_LDY,
        OP_STA, OP_STX, OP_STY, OP_STZ,
        OP_ASL, OP_LSR, OP_ROL, OP_ROR,
        OP_INC, OP_DEC, OP_TSB, OP_TRB, OP_RMB, OP_SMB,
        OP_BPL, OP_BMI, OP_BVC, OP_BVS,
        OP_BCC, OP_BCS, OP_BNE, OP_BEQ, OP_BRA,
        OP_BBR, OP_BBS,
        OP_BRK, OP_JMP, OP_JSR, OP_RTS, OP_RTI,
        OP_PHP, OP_PLP, OP_PHA, OP_PLA,
        OP_PHX, OP_PLX, OP_PHY, OP_PLY,
        OP_CLC, OP_SEC, OP_CLI, OP_SEI, OP_CLV, OP_CLD, OP_SED,
        OP_TAX, OP_TXA, OP_TAY, OP_TYA, OP_TSX, OP_TXS,
        OP_DEX, OP_DEY, OP_INX, OP_INY,
        OP_WAI, OP_STP
    } op_t;

    typedef enum logic [4:0] {
        AM_IMP,
        AM_ACC,
        AM_IMM,
        AM_ZP,
        AM_ZPX,
        AM_ZPY,
        AM_ABS,
        AM_ABSX,
        AM_ABSY,
        AM_INDX,
        AM_INDY,
        AM_ZPIND,
        AM_REL,
        AM_ZPREL,
        AM_ABSIND,
        AM_ABSXIND
    } addr_mode_t;

    typedef enum logic [2:0] {
        KIND_READ,
        KIND_WRITE,
        KIND_RMW,
        KIND_IMPLIED,
        KIND_BRANCH,
        KIND_SPECIAL
    } kind_t;

    typedef struct packed {
        op_t        op;
        addr_mode_t mode;
        logic       one_cycle;
    } decode_t;

    typedef enum logic [6:0] {
        ST_RESET_0,
        ST_RESET_1,
        ST_RESET_STACK_0,
        ST_RESET_STACK_1,
        ST_RESET_STACK_2,
        ST_RESET_VECTOR_LO,
        ST_RESET_VECTOR_HI,

        ST_FETCH,
        ST_IMPLIED,
        ST_OPERAND,
        ST_ABS_HI,
        ST_ZP_INDEX,
        ST_INDX_DUMMY,
        ST_INDEX_DUMMY,
        ST_PTR_LO,
        ST_PTR_HI,
        ST_MEM_READ,
        ST_DECIMAL_EXTRA,
        ST_MEM_WRITE,
        ST_RMW_READ,
        ST_RMW_MODIFY,
        ST_RMW_WRITE,
        ST_NOP_ABS_DUMMY,

        ST_BRANCH_DUMMY,
        ST_BRANCH_CROSS,
        ST_BIT_BRANCH_READ,
        ST_BIT_BRANCH_REPEAT,
        ST_BIT_BRANCH_OFFSET,
        ST_BIT_BRANCH_DUMMY,
        ST_BIT_BRANCH_CROSS,

        ST_PUSH_DUMMY,
        ST_PUSH_WRITE,
        ST_PULL_DUMMY_PC,
        ST_PULL_DUMMY_STACK,
        ST_PULL_READ,

        ST_JSR_LOW,
        ST_JSR_STACK_DUMMY,
        ST_JSR_PUSH_HI,
        ST_JSR_PUSH_LO,
        ST_JSR_HIGH,

        ST_RTS_DUMMY_PC,
        ST_RTS_DUMMY_STACK,
        ST_RTS_PULL_LO,
        ST_RTS_PULL_HI,
        ST_RTS_FINAL,

        ST_RTI_DUMMY_PC,
        ST_RTI_DUMMY_STACK,
        ST_RTI_PULL_P,
        ST_RTI_PULL_LO,
        ST_RTI_PULL_HI,

        ST_BRK_SIGNATURE,
        ST_INT_DUMMY,
        ST_INT_PUSH_HI,
        ST_INT_PUSH_LO,
        ST_INT_PUSH_P,
        ST_INT_VECTOR_LO,
        ST_INT_VECTOR_HI,

        ST_JMP_IND_LO,
        ST_JMP_IND_HI,
        ST_JMP_IND_LAST,
        ST_JMP_X_DUMMY,
        ST_JMP_X_LO,
        ST_JMP_X_HI,

        ST_WAI_DUMMY,
        ST_WAIT,
        ST_STP_DUMMY,
        ST_STOP
    } state_t;

    logic [15:0] pc_q;
    logic [7:0]  s_q;
    logic [7:0]  a_q;
    logic [7:0]  x_q;
    logic [7:0]  y_q;
    logic [7:0]  p_q;
    logic [7:0]  ir_q;

    state_t      state_q;
    op_t         op_q;
    addr_mode_t  mode_q;
    kind_t       kind_q;

    logic [7:0]  operand_q;
    logic [7:0]  lo_q;
    logic [7:0]  hi_q;
    logic [7:0]  data_q;
    logic [7:0]  result_q;
    /* W65C02 decimal ADC/SBC already owns ST_DECIMAL_EXTRA. Save the exact
     * arithmetic result at the operand edge, then commit A/P on that existing
     * extra cycle so the BCD carry chains do not feed the shared flag mux. */
    logic [15:0] decimal_result_q;
    logic        decimal_so_pending_q;
    logic [7:0]  ptr_q;
    logic [15:0] ea_q;
    logic [15:0] target_q;
    logic        page_cross_q;
    logic [15:0] vector_q;

    logic        nmi_last_q;
    logic        nmi_pending_q;
    logic        so_last_q;
    logic        nmi_edge;

    logic [8:0]  index_sum_x;
    logic [8:0]  index_sum_y;
    logic [15:0] relative_base;
    logic [15:0] relative_target;

    assign index_sum_x = {1'b0, lo_q} + {1'b0, x_q};
    assign index_sum_y = {1'b0, lo_q} + {1'b0, y_q};
    assign relative_base = pc_q + 16'd1;
    assign relative_target = relative_base + {{8{data_in[7]}}, data_in};
    assign nmi_edge = nmi_last_q && !nmi_n;

    function automatic kind_t kind_for_op(input op_t op);
        case (op)
            OP_STA, OP_STX, OP_STY, OP_STZ:
                kind_for_op = KIND_WRITE;

            OP_ASL, OP_LSR, OP_ROL, OP_ROR,
            OP_INC, OP_DEC, OP_TSB, OP_TRB, OP_RMB, OP_SMB:
                kind_for_op = KIND_RMW;

            OP_BPL, OP_BMI, OP_BVC, OP_BVS,
            OP_BCC, OP_BCS, OP_BNE, OP_BEQ, OP_BRA,
            OP_BBR, OP_BBS:
                kind_for_op = KIND_BRANCH;

            OP_CLC, OP_SEC, OP_CLI, OP_SEI, OP_CLV, OP_CLD, OP_SED,
            OP_TAX, OP_TXA, OP_TAY, OP_TYA, OP_TSX, OP_TXS,
            OP_DEX, OP_DEY, OP_INX, OP_INY:
                kind_for_op = KIND_IMPLIED;

            OP_BRK, OP_JMP, OP_JSR, OP_RTS, OP_RTI,
            OP_PHP, OP_PLP, OP_PHA, OP_PLA,
            OP_PHX, OP_PLX, OP_PHY, OP_PLY,
            OP_WAI, OP_STP:
                kind_for_op = KIND_SPECIAL;

            default:
                kind_for_op = KIND_READ;
        endcase
    endfunction

    function automatic decode_t decode_opcode(input logic [7:0] opcode);
        decode_t d;
        begin
            d.op = OP_NOP;
            d.mode = AM_IMP;
            d.one_cycle = 1'b0;

            case (opcode)
                8'h00: begin d.op = OP_BRK; d.mode = AM_IMP; end
                8'h01: begin d.op = OP_ORA; d.mode = AM_INDX; end
                8'h02: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'h03: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h04: begin d.op = OP_TSB; d.mode = AM_ZP; end
                8'h05: begin d.op = OP_ORA; d.mode = AM_ZP; end
                8'h06: begin d.op = OP_ASL; d.mode = AM_ZP; end
                8'h07: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h08: begin d.op = OP_PHP; d.mode = AM_IMP; end
                8'h09: begin d.op = OP_ORA; d.mode = AM_IMM; end
                8'h0A: begin d.op = OP_ASL; d.mode = AM_ACC; end
                8'h0B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h0C: begin d.op = OP_TSB; d.mode = AM_ABS; end
                8'h0D: begin d.op = OP_ORA; d.mode = AM_ABS; end
                8'h0E: begin d.op = OP_ASL; d.mode = AM_ABS; end
                8'h0F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h10: begin d.op = OP_BPL; d.mode = AM_REL; end
                8'h11: begin d.op = OP_ORA; d.mode = AM_INDY; end
                8'h12: begin d.op = OP_ORA; d.mode = AM_ZPIND; end
                8'h13: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h14: begin d.op = OP_TRB; d.mode = AM_ZP; end
                8'h15: begin d.op = OP_ORA; d.mode = AM_ZPX; end
                8'h16: begin d.op = OP_ASL; d.mode = AM_ZPX; end
                8'h17: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h18: begin d.op = OP_CLC; d.mode = AM_IMP; end
                8'h19: begin d.op = OP_ORA; d.mode = AM_ABSY; end
                8'h1A: begin d.op = OP_INC; d.mode = AM_ACC; end
                8'h1B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h1C: begin d.op = OP_TRB; d.mode = AM_ABS; end
                8'h1D: begin d.op = OP_ORA; d.mode = AM_ABSX; end
                8'h1E: begin d.op = OP_ASL; d.mode = AM_ABSX; end
                8'h1F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h20: begin d.op = OP_JSR; d.mode = AM_ABS; end
                8'h21: begin d.op = OP_AND; d.mode = AM_INDX; end
                8'h22: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'h23: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h24: begin d.op = OP_BIT; d.mode = AM_ZP; end
                8'h25: begin d.op = OP_AND; d.mode = AM_ZP; end
                8'h26: begin d.op = OP_ROL; d.mode = AM_ZP; end
                8'h27: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h28: begin d.op = OP_PLP; d.mode = AM_IMP; end
                8'h29: begin d.op = OP_AND; d.mode = AM_IMM; end
                8'h2A: begin d.op = OP_ROL; d.mode = AM_ACC; end
                8'h2B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h2C: begin d.op = OP_BIT; d.mode = AM_ABS; end
                8'h2D: begin d.op = OP_AND; d.mode = AM_ABS; end
                8'h2E: begin d.op = OP_ROL; d.mode = AM_ABS; end
                8'h2F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h30: begin d.op = OP_BMI; d.mode = AM_REL; end
                8'h31: begin d.op = OP_AND; d.mode = AM_INDY; end
                8'h32: begin d.op = OP_AND; d.mode = AM_ZPIND; end
                8'h33: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h34: begin d.op = OP_BIT; d.mode = AM_ZPX; end
                8'h35: begin d.op = OP_AND; d.mode = AM_ZPX; end
                8'h36: begin d.op = OP_ROL; d.mode = AM_ZPX; end
                8'h37: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h38: begin d.op = OP_SEC; d.mode = AM_IMP; end
                8'h39: begin d.op = OP_AND; d.mode = AM_ABSY; end
                8'h3A: begin d.op = OP_DEC; d.mode = AM_ACC; end
                8'h3B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h3C: begin d.op = OP_BIT; d.mode = AM_ABSX; end
                8'h3D: begin d.op = OP_AND; d.mode = AM_ABSX; end
                8'h3E: begin d.op = OP_ROL; d.mode = AM_ABSX; end
                8'h3F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h40: begin d.op = OP_RTI; d.mode = AM_IMP; end
                8'h41: begin d.op = OP_EOR; d.mode = AM_INDX; end
                8'h42: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'h43: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h44: begin d.op = OP_NOP; d.mode = AM_ZP; end
                8'h45: begin d.op = OP_EOR; d.mode = AM_ZP; end
                8'h46: begin d.op = OP_LSR; d.mode = AM_ZP; end
                8'h47: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h48: begin d.op = OP_PHA; d.mode = AM_IMP; end
                8'h49: begin d.op = OP_EOR; d.mode = AM_IMM; end
                8'h4A: begin d.op = OP_LSR; d.mode = AM_ACC; end
                8'h4B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h4C: begin d.op = OP_JMP; d.mode = AM_ABS; end
                8'h4D: begin d.op = OP_EOR; d.mode = AM_ABS; end
                8'h4E: begin d.op = OP_LSR; d.mode = AM_ABS; end
                8'h4F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h50: begin d.op = OP_BVC; d.mode = AM_REL; end
                8'h51: begin d.op = OP_EOR; d.mode = AM_INDY; end
                8'h52: begin d.op = OP_EOR; d.mode = AM_ZPIND; end
                8'h53: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h54: begin d.op = OP_NOP; d.mode = AM_ZPX; end
                8'h55: begin d.op = OP_EOR; d.mode = AM_ZPX; end
                8'h56: begin d.op = OP_LSR; d.mode = AM_ZPX; end
                8'h57: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h58: begin d.op = OP_CLI; d.mode = AM_IMP; end
                8'h59: begin d.op = OP_EOR; d.mode = AM_ABSY; end
                8'h5A: begin d.op = OP_PHY; d.mode = AM_IMP; end
                8'h5B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h5C: begin d.op = OP_NOP; d.mode = AM_ABS; end
                8'h5D: begin d.op = OP_EOR; d.mode = AM_ABSX; end
                8'h5E: begin d.op = OP_LSR; d.mode = AM_ABSX; end
                8'h5F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h60: begin d.op = OP_RTS; d.mode = AM_IMP; end
                8'h61: begin d.op = OP_ADC; d.mode = AM_INDX; end
                8'h62: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'h63: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h64: begin d.op = OP_STZ; d.mode = AM_ZP; end
                8'h65: begin d.op = OP_ADC; d.mode = AM_ZP; end
                8'h66: begin d.op = OP_ROR; d.mode = AM_ZP; end
                8'h67: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h68: begin d.op = OP_PLA; d.mode = AM_IMP; end
                8'h69: begin d.op = OP_ADC; d.mode = AM_IMM; end
                8'h6A: begin d.op = OP_ROR; d.mode = AM_ACC; end
                8'h6B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h6C: begin d.op = OP_JMP; d.mode = AM_ABSIND; end
                8'h6D: begin d.op = OP_ADC; d.mode = AM_ABS; end
                8'h6E: begin d.op = OP_ROR; d.mode = AM_ABS; end
                8'h6F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h70: begin d.op = OP_BVS; d.mode = AM_REL; end
                8'h71: begin d.op = OP_ADC; d.mode = AM_INDY; end
                8'h72: begin d.op = OP_ADC; d.mode = AM_ZPIND; end
                8'h73: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h74: begin d.op = OP_STZ; d.mode = AM_ZPX; end
                8'h75: begin d.op = OP_ADC; d.mode = AM_ZPX; end
                8'h76: begin d.op = OP_ROR; d.mode = AM_ZPX; end
                8'h77: begin d.op = OP_RMB; d.mode = AM_ZP; end
                8'h78: begin d.op = OP_SEI; d.mode = AM_IMP; end
                8'h79: begin d.op = OP_ADC; d.mode = AM_ABSY; end
                8'h7A: begin d.op = OP_PLY; d.mode = AM_IMP; end
                8'h7B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h7C: begin d.op = OP_JMP; d.mode = AM_ABSXIND; end
                8'h7D: begin d.op = OP_ADC; d.mode = AM_ABSX; end
                8'h7E: begin d.op = OP_ROR; d.mode = AM_ABSX; end
                8'h7F: begin d.op = OP_BBR; d.mode = AM_ZPREL; end

                8'h80: begin d.op = OP_BRA; d.mode = AM_REL; end
                8'h81: begin d.op = OP_STA; d.mode = AM_INDX; end
                8'h82: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'h83: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h84: begin d.op = OP_STY; d.mode = AM_ZP; end
                8'h85: begin d.op = OP_STA; d.mode = AM_ZP; end
                8'h86: begin d.op = OP_STX; d.mode = AM_ZP; end
                8'h87: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'h88: begin d.op = OP_DEY; d.mode = AM_IMP; end
                8'h89: begin d.op = OP_BIT; d.mode = AM_IMM; end
                8'h8A: begin d.op = OP_TXA; d.mode = AM_IMP; end
                8'h8B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h8C: begin d.op = OP_STY; d.mode = AM_ABS; end
                8'h8D: begin d.op = OP_STA; d.mode = AM_ABS; end
                8'h8E: begin d.op = OP_STX; d.mode = AM_ABS; end
                8'h8F: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'h90: begin d.op = OP_BCC; d.mode = AM_REL; end
                8'h91: begin d.op = OP_STA; d.mode = AM_INDY; end
                8'h92: begin d.op = OP_STA; d.mode = AM_ZPIND; end
                8'h93: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h94: begin d.op = OP_STY; d.mode = AM_ZPX; end
                8'h95: begin d.op = OP_STA; d.mode = AM_ZPX; end
                8'h96: begin d.op = OP_STX; d.mode = AM_ZPY; end
                8'h97: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'h98: begin d.op = OP_TYA; d.mode = AM_IMP; end
                8'h99: begin d.op = OP_STA; d.mode = AM_ABSY; end
                8'h9A: begin d.op = OP_TXS; d.mode = AM_IMP; end
                8'h9B: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'h9C: begin d.op = OP_STZ; d.mode = AM_ABS; end
                8'h9D: begin d.op = OP_STA; d.mode = AM_ABSX; end
                8'h9E: begin d.op = OP_STZ; d.mode = AM_ABSX; end
                8'h9F: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hA0: begin d.op = OP_LDY; d.mode = AM_IMM; end
                8'hA1: begin d.op = OP_LDA; d.mode = AM_INDX; end
                8'hA2: begin d.op = OP_LDX; d.mode = AM_IMM; end
                8'hA3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hA4: begin d.op = OP_LDY; d.mode = AM_ZP; end
                8'hA5: begin d.op = OP_LDA; d.mode = AM_ZP; end
                8'hA6: begin d.op = OP_LDX; d.mode = AM_ZP; end
                8'hA7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hA8: begin d.op = OP_TAY; d.mode = AM_IMP; end
                8'hA9: begin d.op = OP_LDA; d.mode = AM_IMM; end
                8'hAA: begin d.op = OP_TAX; d.mode = AM_IMP; end
                8'hAB: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hAC: begin d.op = OP_LDY; d.mode = AM_ABS; end
                8'hAD: begin d.op = OP_LDA; d.mode = AM_ABS; end
                8'hAE: begin d.op = OP_LDX; d.mode = AM_ABS; end
                8'hAF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hB0: begin d.op = OP_BCS; d.mode = AM_REL; end
                8'hB1: begin d.op = OP_LDA; d.mode = AM_INDY; end
                8'hB2: begin d.op = OP_LDA; d.mode = AM_ZPIND; end
                8'hB3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hB4: begin d.op = OP_LDY; d.mode = AM_ZPX; end
                8'hB5: begin d.op = OP_LDA; d.mode = AM_ZPX; end
                8'hB6: begin d.op = OP_LDX; d.mode = AM_ZPY; end
                8'hB7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hB8: begin d.op = OP_CLV; d.mode = AM_IMP; end
                8'hB9: begin d.op = OP_LDA; d.mode = AM_ABSY; end
                8'hBA: begin d.op = OP_TSX; d.mode = AM_IMP; end
                8'hBB: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hBC: begin d.op = OP_LDY; d.mode = AM_ABSX; end
                8'hBD: begin d.op = OP_LDA; d.mode = AM_ABSX; end
                8'hBE: begin d.op = OP_LDX; d.mode = AM_ABSY; end
                8'hBF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hC0: begin d.op = OP_CPY; d.mode = AM_IMM; end
                8'hC1: begin d.op = OP_CMP; d.mode = AM_INDX; end
                8'hC2: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'hC3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hC4: begin d.op = OP_CPY; d.mode = AM_ZP; end
                8'hC5: begin d.op = OP_CMP; d.mode = AM_ZP; end
                8'hC6: begin d.op = OP_DEC; d.mode = AM_ZP; end
                8'hC7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hC8: begin d.op = OP_INY; d.mode = AM_IMP; end
                8'hC9: begin d.op = OP_CMP; d.mode = AM_IMM; end
                8'hCA: begin d.op = OP_DEX; d.mode = AM_IMP; end
                8'hCB: begin d.op = OP_WAI; d.mode = AM_IMP; end
                8'hCC: begin d.op = OP_CPY; d.mode = AM_ABS; end
                8'hCD: begin d.op = OP_CMP; d.mode = AM_ABS; end
                8'hCE: begin d.op = OP_DEC; d.mode = AM_ABS; end
                8'hCF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hD0: begin d.op = OP_BNE; d.mode = AM_REL; end
                8'hD1: begin d.op = OP_CMP; d.mode = AM_INDY; end
                8'hD2: begin d.op = OP_CMP; d.mode = AM_ZPIND; end
                8'hD3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hD4: begin d.op = OP_NOP; d.mode = AM_ZPX; end
                8'hD5: begin d.op = OP_CMP; d.mode = AM_ZPX; end
                8'hD6: begin d.op = OP_DEC; d.mode = AM_ZPX; end
                8'hD7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hD8: begin d.op = OP_CLD; d.mode = AM_IMP; end
                8'hD9: begin d.op = OP_CMP; d.mode = AM_ABSY; end
                8'hDA: begin d.op = OP_PHX; d.mode = AM_IMP; end
                8'hDB: begin d.op = OP_STP; d.mode = AM_IMP; end
                8'hDC: begin d.op = OP_NOP; d.mode = AM_ABS; end
                8'hDD: begin d.op = OP_CMP; d.mode = AM_ABSX; end
                8'hDE: begin d.op = OP_DEC; d.mode = AM_ABSX; end
                8'hDF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hE0: begin d.op = OP_CPX; d.mode = AM_IMM; end
                8'hE1: begin d.op = OP_SBC; d.mode = AM_INDX; end
                8'hE2: begin d.op = OP_NOP; d.mode = AM_IMM; end
                8'hE3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hE4: begin d.op = OP_CPX; d.mode = AM_ZP; end
                8'hE5: begin d.op = OP_SBC; d.mode = AM_ZP; end
                8'hE6: begin d.op = OP_INC; d.mode = AM_ZP; end
                8'hE7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hE8: begin d.op = OP_INX; d.mode = AM_IMP; end
                8'hE9: begin d.op = OP_SBC; d.mode = AM_IMM; end
                8'hEA: begin d.op = OP_NOP; d.mode = AM_IMP; end
                8'hEB: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hEC: begin d.op = OP_CPX; d.mode = AM_ABS; end
                8'hED: begin d.op = OP_SBC; d.mode = AM_ABS; end
                8'hEE: begin d.op = OP_INC; d.mode = AM_ABS; end
                8'hEF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end

                8'hF0: begin d.op = OP_BEQ; d.mode = AM_REL; end
                8'hF1: begin d.op = OP_SBC; d.mode = AM_INDY; end
                8'hF2: begin d.op = OP_SBC; d.mode = AM_ZPIND; end
                8'hF3: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hF4: begin d.op = OP_NOP; d.mode = AM_ZPX; end
                8'hF5: begin d.op = OP_SBC; d.mode = AM_ZPX; end
                8'hF6: begin d.op = OP_INC; d.mode = AM_ZPX; end
                8'hF7: begin d.op = OP_SMB; d.mode = AM_ZP; end
                8'hF8: begin d.op = OP_SED; d.mode = AM_IMP; end
                8'hF9: begin d.op = OP_SBC; d.mode = AM_ABSY; end
                8'hFA: begin d.op = OP_PLX; d.mode = AM_IMP; end
                8'hFB: begin d.op = OP_NOP; d.mode = AM_IMP; d.one_cycle = 1'b1; end
                8'hFC: begin d.op = OP_NOP; d.mode = AM_ABS; end
                8'hFD: begin d.op = OP_SBC; d.mode = AM_ABSX; end
                8'hFE: begin d.op = OP_INC; d.mode = AM_ABSX; end
                8'hFF: begin d.op = OP_BBS; d.mode = AM_ZPREL; end
                default: begin end
            endcase

            decode_opcode = d;
        end
    endfunction

    function automatic logic [7:0] normalize_p(input logic [7:0] value);
        normalize_p = value | 8'h20;
    endfunction

    function automatic logic [7:0] with_nz(
        input logic [7:0] flags,
        input logic [7:0] value
    );
        logic [7:0] next_flags;
        begin
            next_flags = normalize_p(flags);
            next_flags[P_N] = value[7];
            next_flags[P_Z] = (value == 8'h00);
            with_nz = next_flags;
        end
    endfunction

    // {new P, new A}.  W65C02S decimal mode updates N and Z from the
    // adjusted result and consumes one additional bus cycle.
    function automatic logic [15:0] adc_result(
        input logic [7:0] accum,
        input logic [7:0] value,
        input logic [7:0] flags
    );
        logic [8:0] binary_sum;
        logic [5:0] low_digit;
        logic [5:0] high_digit;
        logic       low_carry;
        logic       decimal_carry;
        logic       overflow;
        logic [7:0] next_a;
        logic [7:0] next_p;
        begin
            binary_sum = {1'b0, accum} + {1'b0, value} + flags[P_C];

            if (flags[P_D]) begin
                // Correct each digit independently.  Comparing the combined
                // byte against $99 is wrong for invalid BCD operands: for
                // example, $8f + $04 + 1 is $9a, not $fa.
                low_digit = {2'b00, accum[3:0]} +
                            {2'b00, value[3:0]} + flags[P_C];
                low_carry = (low_digit > 6'd9);
                if (low_carry)
                    low_digit = low_digit + 6'd6;

                high_digit = {2'b00, accum[7:4]} +
                             {2'b00, value[7:4]} + low_carry;
                // The W65C02 derives V after low-digit correction but before
                // high-digit correction.  This differs from a plain binary
                // overflow test for invalid BCD inputs such as $3a + $42.
                overflow = (~(accum[7] ^ value[7])) &
                           (accum[7] ^ high_digit[3]);
                decimal_carry = (high_digit > 6'd9);
                if (decimal_carry)
                    high_digit = high_digit + 6'd6;
                next_a = {high_digit[3:0], low_digit[3:0]};
            end else begin
                decimal_carry = binary_sum[8];
                overflow = (~(accum[7] ^ value[7])) &
                           (accum[7] ^ binary_sum[7]);
                next_a = binary_sum[7:0];
            end

            next_p = with_nz(flags, next_a);
            next_p[P_C] = decimal_carry;
            next_p[P_V] = overflow;
            adc_result = {next_p, next_a};
        end
    endfunction

    function automatic logic [15:0] sbc_result(
        input logic [7:0] accum,
        input logic [7:0] value,
        input logic [7:0] flags
    );
        logic signed [9:0] binary_diff;
        logic signed [5:0] low_binary;
        logic signed [9:0] decimal_stage;
        logic              low_borrow;
        logic [7:0]        decimal_value;
        logic [7:0] next_a;
        logic [7:0] next_p;
        begin
            binary_diff = $signed({1'b0, accum}) -
                          $signed({1'b0, value}) -
                          (flags[P_C] ? 10'sd0 : 10'sd1);

            if (flags[P_D]) begin
                // Model the two CMOS decimal correction stages explicitly.
                // The final low-digit correction is still required after a
                // high-digit borrow for invalid BCD operands (for example,
                // $00 - $0b produces $8f rather than $9f).
                low_binary = $signed({1'b0, accum[3:0]}) -
                             $signed({1'b0, value[3:0]}) -
                             (flags[P_C] ? 6'sd0 : 6'sd1);
                low_borrow = (low_binary < 0);

                if (low_borrow) begin
                    decimal_stage =
                        $signed({2'b00, accum[7:4], low_binary[3:0]}) -
                        $signed({2'b00, value[7:4], 4'hF}) - 10'sd1;
                end else begin
                    decimal_stage =
                        $signed({2'b00, accum[7:4], low_binary[3:0]}) -
                        $signed({2'b00, value[7:4], 4'h0});
                end

                decimal_value = decimal_stage[7:0];
                if (decimal_stage < 0)
                    decimal_value = decimal_value - 8'h60;
                if (low_borrow)
                    decimal_value = decimal_value - 8'h06;
                next_a = decimal_value;
            end else begin
                next_a = binary_diff[7:0];
            end

            next_p = with_nz(flags, next_a);
            next_p[P_C] = !binary_diff[9];
            next_p[P_V] = (accum[7] ^ value[7]) &
                          (accum[7] ^ binary_diff[7]);
            sbc_result = {next_p, next_a};
        end
    endfunction

    function automatic logic branch_taken(input op_t op, input logic [7:0] flags);
        case (op)
            OP_BPL: branch_taken = !flags[P_N];
            OP_BMI: branch_taken =  flags[P_N];
            OP_BVC: branch_taken = !flags[P_V];
            OP_BVS: branch_taken =  flags[P_V];
            OP_BCC: branch_taken = !flags[P_C];
            OP_BCS: branch_taken =  flags[P_C];
            OP_BNE: branch_taken = !flags[P_Z];
            OP_BEQ: branch_taken =  flags[P_Z];
            OP_BRA: branch_taken = 1'b1;
            default: branch_taken = 1'b0;
        endcase
    endfunction

    function automatic logic [7:0] store_value(input op_t op);
        case (op)
            OP_STA: store_value = a_q;
            OP_STX: store_value = x_q;
            OP_STY: store_value = y_q;
            default: store_value = 8'h00;
        endcase
    endfunction

    function automatic logic [7:0] stack_push_value(input op_t op);
        case (op)
            OP_PHP: stack_push_value = p_q | 8'h30;
            OP_PHA: stack_push_value = a_q;
            OP_PHX: stack_push_value = x_q;
            OP_PHY: stack_push_value = y_q;
            default: stack_push_value = 8'h00;
        endcase
    endfunction

    task automatic apply_read_value(input logic [7:0] value);
        logic [8:0] compare_value;
        logic [15:0] arithmetic;
        logic [7:0] next_value;
        begin
            case (op_q)
                OP_ORA: begin
                    next_value = a_q | value;
                    a_q <= next_value;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_AND: begin
                    next_value = a_q & value;
                    a_q <= next_value;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_EOR: begin
                    next_value = a_q ^ value;
                    a_q <= next_value;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_ADC: begin
                    arithmetic = adc_result(a_q, value, p_q);
                    p_q <= arithmetic[15:8];
                    a_q <= arithmetic[7:0];
                end
                OP_SBC: begin
                    arithmetic = sbc_result(a_q, value, p_q);
                    p_q <= arithmetic[15:8];
                    a_q <= arithmetic[7:0];
                end
                OP_CMP: begin
                    compare_value = {1'b0, a_q} - {1'b0, value};
                    p_q <= with_nz({p_q[7:1], (a_q >= value)}, compare_value[7:0]);
                end
                OP_CPX: begin
                    compare_value = {1'b0, x_q} - {1'b0, value};
                    p_q <= with_nz({p_q[7:1], (x_q >= value)}, compare_value[7:0]);
                end
                OP_CPY: begin
                    compare_value = {1'b0, y_q} - {1'b0, value};
                    p_q <= with_nz({p_q[7:1], (y_q >= value)}, compare_value[7:0]);
                end
                OP_BIT: begin
                    p_q[P_Z] <= ((a_q & value) == 8'h00);
                    if (mode_q != AM_IMM) begin
                        p_q[P_N] <= value[7];
                        p_q[P_V] <= value[6];
                    end
                end
                OP_LDA: begin
                    a_q <= value;
                    p_q <= with_nz(p_q, value);
                end
                OP_LDX: begin
                    x_q <= value;
                    p_q <= with_nz(p_q, value);
                end
                OP_LDY: begin
                    y_q <= value;
                    p_q <= with_nz(p_q, value);
                end
                default: begin end
            endcase
        end
    endtask

    task automatic apply_implied;
        logic [7:0] next_value;
        begin
            case (op_q)
                OP_CLC: p_q[P_C] <= 1'b0;
                OP_SEC: p_q[P_C] <= 1'b1;
                OP_CLI: p_q[P_I] <= 1'b0;
                OP_SEI: p_q[P_I] <= 1'b1;
                OP_CLV: p_q[P_V] <= 1'b0;
                OP_CLD: p_q[P_D] <= 1'b0;
                OP_SED: p_q[P_D] <= 1'b1;

                OP_TAX: begin x_q <= a_q; p_q <= with_nz(p_q, a_q); end
                OP_TXA: begin a_q <= x_q; p_q <= with_nz(p_q, x_q); end
                OP_TAY: begin y_q <= a_q; p_q <= with_nz(p_q, a_q); end
                OP_TYA: begin a_q <= y_q; p_q <= with_nz(p_q, y_q); end
                OP_TSX: begin x_q <= s_q; p_q <= with_nz(p_q, s_q); end
                OP_TXS: s_q <= x_q;

                OP_DEX: begin next_value = x_q - 8'd1; x_q <= next_value; p_q <= with_nz(p_q, next_value); end
                OP_DEY: begin next_value = y_q - 8'd1; y_q <= next_value; p_q <= with_nz(p_q, next_value); end
                OP_INX: begin next_value = x_q + 8'd1; x_q <= next_value; p_q <= with_nz(p_q, next_value); end
                OP_INY: begin next_value = y_q + 8'd1; y_q <= next_value; p_q <= with_nz(p_q, next_value); end

                OP_ASL: begin
                    next_value = {a_q[6:0], 1'b0};
                    a_q <= next_value;
                    p_q <= with_nz({p_q[7:1], a_q[7]}, next_value);
                end
                OP_LSR: begin
                    next_value = {1'b0, a_q[7:1]};
                    a_q <= next_value;
                    p_q <= with_nz({p_q[7:1], a_q[0]}, next_value);
                end
                OP_ROL: begin
                    next_value = {a_q[6:0], p_q[P_C]};
                    a_q <= next_value;
                    p_q <= with_nz({p_q[7:1], a_q[7]}, next_value);
                end
                OP_ROR: begin
                    next_value = {p_q[P_C], a_q[7:1]};
                    a_q <= next_value;
                    p_q <= with_nz({p_q[7:1], a_q[0]}, next_value);
                end
                OP_INC: begin
                    next_value = a_q + 8'd1;
                    a_q <= next_value;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_DEC: begin
                    next_value = a_q - 8'd1;
                    a_q <= next_value;
                    p_q <= with_nz(p_q, next_value);
                end
                default: begin end
            endcase
        end
    endtask

    task automatic prepare_rmw(input logic [7:0] value);
        logic [7:0] next_value;
        begin
            next_value = value;
            case (op_q)
                OP_ASL: begin
                    next_value = {value[6:0], 1'b0};
                    p_q <= with_nz({p_q[7:1], value[7]}, next_value);
                end
                OP_LSR: begin
                    next_value = {1'b0, value[7:1]};
                    p_q <= with_nz({p_q[7:1], value[0]}, next_value);
                end
                OP_ROL: begin
                    next_value = {value[6:0], p_q[P_C]};
                    p_q <= with_nz({p_q[7:1], value[7]}, next_value);
                end
                OP_ROR: begin
                    next_value = {p_q[P_C], value[7:1]};
                    p_q <= with_nz({p_q[7:1], value[0]}, next_value);
                end
                OP_INC: begin
                    next_value = value + 8'd1;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_DEC: begin
                    next_value = value - 8'd1;
                    p_q <= with_nz(p_q, next_value);
                end
                OP_TSB: begin
                    next_value = value | a_q;
                    p_q[P_Z] <= ((value & a_q) == 8'h00);
                end
                OP_TRB: begin
                    next_value = value & ~a_q;
                    p_q[P_Z] <= ((value & a_q) == 8'h00);
                end
                OP_RMB: next_value = value & ~(8'h01 << ir_q[6:4]);
                OP_SMB: next_value = value |  (8'h01 << ir_q[6:4]);
                default: begin end
            endcase
            result_q <= next_value;
        end
    endtask

    decode_t fetch_decode;
    assign fetch_decode = decode_opcode(data_in);

    assign debug_pc = pc_q;
    assign debug_s  = s_q;
    assign debug_a  = a_q;
    assign debug_x  = x_q;
    assign debug_y  = y_q;
    assign debug_p  = p_q;

    always_comb begin
        addr = pc_q;
        data_out = 8'h00;
        rwb = 1'b1;
        sync = 1'b0;
        vpb_n = 1'b1;
        mlb_n = 1'b1;
        waiting = (state_q == ST_WAIT);
        stopped = (state_q == ST_STOP);

        case (state_q)
            ST_RESET_STACK_0,
            ST_RESET_STACK_1,
            ST_RESET_STACK_2:
                addr = {8'h01, s_q};

            ST_RESET_VECTOR_LO: begin addr = 16'hFFFC; vpb_n = 1'b0; end
            ST_RESET_VECTOR_HI: begin addr = 16'hFFFD; vpb_n = 1'b0; end

            ST_FETCH: begin
                addr = pc_q;
                sync = 1'b1;
            end

            ST_OPERAND,
            ST_ABS_HI,
            ST_IMPLIED,
            ST_BRANCH_DUMMY,
            ST_BIT_BRANCH_OFFSET,
            ST_BIT_BRANCH_DUMMY,
            ST_BIT_BRANCH_CROSS,
            ST_PUSH_DUMMY,
            ST_PULL_DUMMY_PC,
            ST_JSR_LOW,
            ST_RTS_DUMMY_PC,
            ST_RTI_DUMMY_PC,
            ST_BRK_SIGNATURE,
            ST_INT_DUMMY,
            ST_WAI_DUMMY,
            ST_STP_DUMMY:
                addr = pc_q;

            ST_BRANCH_CROSS:
                addr = {pc_q[15:8], target_q[7:0]};

            ST_ZP_INDEX,
            ST_INDX_DUMMY:
                addr = {8'h00, operand_q};

            ST_INDEX_DUMMY: begin
                // Apple-compatible 65C02s retain the NMOS effective-address
                // false read for same-page STA abs,X/abs,Y.  On a page
                // crossing, the CMOS cycle instead reads the final
                // instruction byte.
                if ((op_q == OP_STA) &&
                    (mode_q == AM_ABSX || mode_q == AM_ABSY) &&
                    !page_cross_q)
                    addr = ea_q;
                else
                    addr = pc_q - 16'd1;
            end

            ST_NOP_ABS_DUMMY:
                addr = pc_q - 16'd1;

            ST_PTR_LO:
                addr = {8'h00, ptr_q};
            ST_PTR_HI:
                addr = {8'h00, ptr_q + 8'd1};

            ST_MEM_READ,
            ST_DECIMAL_EXTRA,
            ST_RMW_READ:
                addr = ea_q;

            ST_MEM_WRITE: begin
                addr = ea_q;
                data_out = store_value(op_q);
                rwb = 1'b0;
            end

            ST_RMW_MODIFY: begin
                addr = ea_q;
                mlb_n = 1'b0;
            end
            ST_RMW_WRITE: begin
                addr = ea_q;
                data_out = result_q;
                rwb = 1'b0;
                mlb_n = 1'b0;
            end

            ST_BIT_BRANCH_READ,
            ST_BIT_BRANCH_REPEAT:
                addr = {8'h00, operand_q};

            ST_PUSH_WRITE: begin
                addr = {8'h01, s_q};
                data_out = stack_push_value(op_q);
                rwb = 1'b0;
            end

            ST_PULL_DUMMY_STACK,
            ST_RTS_DUMMY_STACK,
            ST_RTI_DUMMY_STACK:
                addr = {8'h01, s_q};

            ST_PULL_READ,
            ST_RTS_PULL_LO,
            ST_RTS_PULL_HI,
            ST_RTI_PULL_P,
            ST_RTI_PULL_LO,
            ST_RTI_PULL_HI:
                addr = {8'h01, s_q};

            ST_JSR_STACK_DUMMY:
                addr = {8'h01, s_q};
            ST_JSR_PUSH_HI: begin
                addr = {8'h01, s_q};
                data_out = pc_q[15:8];
                rwb = 1'b0;
            end
            ST_JSR_PUSH_LO: begin
                addr = {8'h01, s_q};
                data_out = pc_q[7:0];
                rwb = 1'b0;
            end
            ST_JSR_HIGH:
                addr = pc_q;

            ST_RTS_FINAL:
                addr = pc_q;

            ST_INT_PUSH_HI: begin
                addr = {8'h01, s_q};
                data_out = pc_q[15:8];
                rwb = 1'b0;
            end
            ST_INT_PUSH_LO: begin
                addr = {8'h01, s_q};
                data_out = pc_q[7:0];
                rwb = 1'b0;
            end
            ST_INT_PUSH_P: begin
                addr = {8'h01, s_q};
                data_out = (op_q == OP_BRK) ? (p_q | 8'h30) : (p_q | 8'h20);
                data_out[4] = (op_q == OP_BRK);
                rwb = 1'b0;
            end
            ST_INT_VECTOR_LO: begin addr = vector_q; vpb_n = 1'b0; end
            ST_INT_VECTOR_HI: begin addr = vector_q + 16'd1; vpb_n = 1'b0; end

            ST_JMP_IND_LO:
                addr = ea_q;
            ST_JMP_IND_HI:
                addr = {ea_q[15:8], ea_q[7:0] + 8'd1};
            ST_JMP_IND_LAST:
                addr = ea_q + 16'd1;
            ST_JMP_X_DUMMY:
                addr = pc_q - 16'd2;
            ST_JMP_X_LO:
                addr = ea_q;
            ST_JMP_X_HI:
                addr = ea_q + 16'd1;

            ST_WAIT,
            ST_STOP:
                addr = pc_q;

            default: begin end
        endcase

        // The W65C02S decimal correction cycle repeats the effective-address
        // read.  Immediate ADC/SBC use the stable internal zero-page dummy
        // addresses represented by the WDC single-step reference model.
        if (state_q == ST_DECIMAL_EXTRA && mode_q == AM_IMM) begin
            addr = (op_q == OP_ADC) ? 16'h007F : 16'h0000;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            pc_q <= 16'h0000;
            s_q <= 8'hFF;
            a_q <= 8'h00;
            x_q <= 8'h00;
            y_q <= 8'h00;
            p_q <= 8'h24;
            ir_q <= 8'h00;
            state_q <= ST_RESET_0;
            op_q <= OP_NOP;
            mode_q <= AM_IMP;
            kind_q <= KIND_IMPLIED;
            operand_q <= 8'h00;
            lo_q <= 8'h00;
            hi_q <= 8'h00;
            data_q <= 8'h00;
            result_q <= 8'h00;
            decimal_result_q <= 16'h0000;
            decimal_so_pending_q <= 1'b0;
            ptr_q <= 8'h00;
            ea_q <= 16'h0000;
            target_q <= 16'h0000;
            page_cross_q <= 1'b0;
            vector_q <= 16'hFFFE;
            nmi_last_q <= 1'b1;
            nmi_pending_q <= 1'b0;
            so_last_q <= 1'b1;
            instruction_done <= 1'b0;
        end else if (DEBUG_STATE_LOAD && debug_load) begin
            pc_q <= debug_pc_in;
            s_q <= debug_s_in;
            a_q <= debug_a_in;
            x_q <= debug_x_in;
            y_q <= debug_y_in;
            p_q <= normalize_p(debug_p_in);
            ir_q <= 8'h00;
            state_q <= ST_FETCH;
            op_q <= OP_NOP;
            mode_q <= AM_IMP;
            kind_q <= KIND_IMPLIED;
            operand_q <= 8'h00;
            lo_q <= 8'h00;
            hi_q <= 8'h00;
            data_q <= 8'h00;
            result_q <= 8'h00;
            decimal_result_q <= 16'h0000;
            decimal_so_pending_q <= 1'b0;
            ptr_q <= 8'h00;
            ea_q <= 16'h0000;
            target_q <= 16'h0000;
            page_cross_q <= 1'b0;
            vector_q <= 16'hFFFE;
            nmi_last_q <= nmi_n;
            nmi_pending_q <= 1'b0;
            so_last_q <= so_n;
            instruction_done <= 1'b0;
        end else begin
            instruction_done <= 1'b0;
            nmi_last_q <= nmi_n;
            so_last_q <= so_n;

            if (nmi_edge)
                nmi_pending_q <= 1'b1;
            if (so_last_q && !so_n) begin
                p_q[P_V] <= 1'b1;
                /* Arithmetic wins when SO falls on the operand edge, as it
                 * did before the retime. During the extra cycle SO wins,
                 * including while RDY or enable holds that cycle. */
                if (state_q == ST_DECIMAL_EXTRA)
                    decimal_so_pending_q <= 1'b1;
            end

            if (enable && ready) begin
                case (state_q)
                    ST_RESET_0: state_q <= ST_RESET_1;
                    ST_RESET_1: state_q <= ST_RESET_STACK_0;
                    ST_RESET_STACK_0: begin s_q <= s_q - 8'd1; state_q <= ST_RESET_STACK_1; end
                    ST_RESET_STACK_1: begin s_q <= s_q - 8'd1; state_q <= ST_RESET_STACK_2; end
                    ST_RESET_STACK_2: begin s_q <= s_q - 8'd1; state_q <= ST_RESET_VECTOR_LO; end
                    ST_RESET_VECTOR_LO: begin lo_q <= data_in; state_q <= ST_RESET_VECTOR_HI; end
                    ST_RESET_VECTOR_HI: begin
                        pc_q <= {data_in, lo_q};
                        p_q[P_I] <= 1'b1;
                        p_q[P_D] <= 1'b0;
                        state_q <= ST_FETCH;
                    end

                    ST_FETCH: begin
                        ir_q <= data_in;
                        op_q <= fetch_decode.op;
                        mode_q <= fetch_decode.mode;
                        kind_q <= kind_for_op(fetch_decode.op);

                        if (nmi_pending_q || nmi_edge ||
                            (!irq_n && !p_q[P_I])) begin
                            op_q <= OP_NOP;
                            vector_q <= (nmi_pending_q || nmi_edge)
                                      ? 16'hFFFA : 16'hFFFE;
                            nmi_pending_q <= 1'b0;
                            state_q <= ST_INT_DUMMY;
                        end else begin
                            pc_q <= pc_q + 16'd1;

                            if (fetch_decode.one_cycle) begin
                                instruction_done <= 1'b1;
                                state_q <= ST_FETCH;
                            end else begin
                                case (fetch_decode.op)
                                    OP_BRK: begin
                                        vector_q <= 16'hFFFE;
                                        state_q <= ST_BRK_SIGNATURE;
                                    end
                                    OP_JSR: state_q <= ST_JSR_LOW;
                                    OP_RTS: state_q <= ST_RTS_DUMMY_PC;
                                    OP_RTI: state_q <= ST_RTI_DUMMY_PC;
                                    OP_PHP, OP_PHA, OP_PHX, OP_PHY:
                                        state_q <= ST_PUSH_DUMMY;
                                    OP_PLP, OP_PLA, OP_PLX, OP_PLY:
                                        state_q <= ST_PULL_DUMMY_PC;
                                    OP_WAI: state_q <= ST_WAI_DUMMY;
                                    OP_STP: state_q <= ST_STP_DUMMY;
                                    default: begin
                                        case (fetch_decode.mode)
                                            AM_IMP, AM_ACC: state_q <= ST_IMPLIED;
                                            default: state_q <= ST_OPERAND;
                                        endcase
                                    end
                                endcase
                            end
                        end
                    end

                    ST_IMPLIED: begin
                        apply_implied();
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_OPERAND: begin
                        operand_q <= data_in;
                        pc_q <= pc_q + 16'd1;

                        case (mode_q)
                            AM_IMM: begin
                                if ((op_q == OP_ADC || op_q == OP_SBC) && p_q[P_D]) begin
                                    if (op_q == OP_ADC)
                                        decimal_result_q <= adc_result(a_q, data_in, p_q);
                                    else
                                        decimal_result_q <= sbc_result(a_q, data_in, p_q);
                                    decimal_so_pending_q <= 1'b0;
                                    ea_q <= (op_q == OP_ADC) ? 16'h007F : 16'h0000;
                                    state_q <= ST_DECIMAL_EXTRA;
                                end else begin
                                    apply_read_value(data_in);
                                    instruction_done <= 1'b1;
                                    state_q <= ST_FETCH;
                                end
                            end

                            AM_ZP: begin
                                ea_q <= {8'h00, data_in};
                                if (op_q == OP_BBR || op_q == OP_BBS)
                                    state_q <= ST_BIT_BRANCH_READ;
                                else if (kind_q == KIND_WRITE)
                                    state_q <= ST_MEM_WRITE;
                                else if (kind_q == KIND_RMW)
                                    state_q <= ST_RMW_READ;
                                else
                                    state_q <= ST_MEM_READ;
                            end

                            AM_ZPX, AM_ZPY: state_q <= ST_ZP_INDEX;

                            AM_ABS, AM_ABSX, AM_ABSY,
                            AM_ABSIND, AM_ABSXIND: begin
                                lo_q <= data_in;
                                state_q <= ST_ABS_HI;
                            end

                            AM_INDX: state_q <= ST_INDX_DUMMY;

                            AM_INDY, AM_ZPIND: begin
                                ptr_q <= data_in;
                                state_q <= ST_PTR_LO;
                            end

                            AM_REL: begin
                                target_q <= relative_target;
                                page_cross_q <= relative_base[15:8] != relative_target[15:8];
                                if (branch_taken(op_q, p_q))
                                    state_q <= ST_BRANCH_DUMMY;
                                else begin
                                    instruction_done <= 1'b1;
                                    state_q <= ST_FETCH;
                                end
                            end

                            AM_ZPREL: state_q <= ST_BIT_BRANCH_READ;

                            default: begin
                                instruction_done <= 1'b1;
                                state_q <= ST_FETCH;
                            end
                        endcase
                    end

                    ST_ABS_HI: begin
                        hi_q <= data_in;
                        pc_q <= pc_q + 16'd1;

                        if (op_q == OP_JMP && mode_q == AM_ABS) begin
                            pc_q <= {data_in, lo_q};
                            instruction_done <= 1'b1;
                            state_q <= ST_FETCH;
                        end else if (op_q == OP_JMP && mode_q == AM_ABSIND) begin
                            ea_q <= {data_in, lo_q};
                            state_q <= ST_JMP_IND_LO;
                        end else if (op_q == OP_JMP && mode_q == AM_ABSXIND) begin
                            ea_q <= {data_in, lo_q} + {8'h00, x_q};
                            state_q <= ST_JMP_X_DUMMY;
                        end else if (op_q == OP_NOP && mode_q == AM_ABS) begin
                            state_q <= ST_NOP_ABS_DUMMY;
                        end else if (mode_q == AM_ABS) begin
                            ea_q <= {data_in, lo_q};
                            if (kind_q == KIND_WRITE)
                                state_q <= ST_MEM_WRITE;
                            else if (kind_q == KIND_RMW)
                                state_q <= ST_RMW_READ;
                            else
                                state_q <= ST_MEM_READ;
                        end else begin
                            if (mode_q == AM_ABSX)
                                ea_q <= {data_in, lo_q} + {8'h00, x_q};
                            else
                                ea_q <= {data_in, lo_q} + {8'h00, y_q};

                            if (mode_q == AM_ABSX)
                                page_cross_q <= index_sum_x[8];
                            else
                                page_cross_q <= index_sum_y[8];

                            if (kind_q == KIND_WRITE ||
                                ((kind_q == KIND_RMW) &&
                                 (op_q == OP_INC || op_q == OP_DEC)) ||
                                ((mode_q == AM_ABSX) && index_sum_x[8]) ||
                                ((mode_q == AM_ABSY) && index_sum_y[8])) begin
                                state_q <= ST_INDEX_DUMMY;
                            end else if (kind_q == KIND_RMW) begin
                                state_q <= ST_RMW_READ;
                            end else begin
                                state_q <= ST_MEM_READ;
                            end
                        end
                    end

                    ST_ZP_INDEX: begin
                        if (mode_q == AM_ZPY)
                            ea_q <= {8'h00, operand_q + y_q};
                        else
                            ea_q <= {8'h00, operand_q + x_q};

                        if (kind_q == KIND_WRITE)
                            state_q <= ST_MEM_WRITE;
                        else if (kind_q == KIND_RMW)
                            state_q <= ST_RMW_READ;
                        else
                            state_q <= ST_MEM_READ;
                    end

                    ST_INDX_DUMMY: begin
                        ptr_q <= operand_q + x_q;
                        state_q <= ST_PTR_LO;
                    end

                    ST_PTR_LO: begin
                        lo_q <= data_in;
                        state_q <= ST_PTR_HI;
                    end

                    ST_PTR_HI: begin
                        hi_q <= data_in;
                        if (mode_q == AM_INDY) begin
                            ea_q <= {data_in, lo_q} + {8'h00, y_q};
                            page_cross_q <= index_sum_y[8];
                            if (kind_q == KIND_WRITE || index_sum_y[8])
                                state_q <= ST_INDEX_DUMMY;
                            else
                                state_q <= ST_MEM_READ;
                        end else begin
                            ea_q <= {data_in, lo_q};
                            if (kind_q == KIND_WRITE)
                                state_q <= ST_MEM_WRITE;
                            else
                                state_q <= ST_MEM_READ;
                        end
                    end

                    ST_INDEX_DUMMY: begin
                        if (kind_q == KIND_WRITE)
                            state_q <= ST_MEM_WRITE;
                        else if (kind_q == KIND_RMW)
                            state_q <= ST_RMW_READ;
                        else
                            state_q <= ST_MEM_READ;
                    end

                    ST_MEM_READ: begin
                        if ((op_q == OP_ADC || op_q == OP_SBC) && p_q[P_D]) begin
                            if (op_q == OP_ADC)
                                decimal_result_q <= adc_result(a_q, data_in, p_q);
                            else
                                decimal_result_q <= sbc_result(a_q, data_in, p_q);
                            decimal_so_pending_q <= 1'b0;
                            state_q <= ST_DECIMAL_EXTRA;
                        end else begin
                            apply_read_value(data_in);
                            instruction_done <= 1'b1;
                            state_q <= ST_FETCH;
                        end
                    end

                    ST_DECIMAL_EXTRA: begin
                        a_q <= decimal_result_q[7:0];
                        p_q <= decimal_result_q[15:8];
                        if (decimal_so_pending_q || (so_last_q && !so_n))
                            p_q[P_V] <= 1'b1;
                        decimal_so_pending_q <= 1'b0;
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_MEM_WRITE: begin
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_RMW_READ: begin
                        data_q <= data_in;
                        prepare_rmw(data_in);
                        state_q <= ST_RMW_MODIFY;
                    end
                    ST_RMW_MODIFY: state_q <= ST_RMW_WRITE;
                    ST_RMW_WRITE: begin
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_NOP_ABS_DUMMY: begin
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_BRANCH_DUMMY: begin
                        if (page_cross_q)
                            state_q <= ST_BRANCH_CROSS;
                        else begin
                            pc_q <= target_q;
                            instruction_done <= 1'b1;
                            state_q <= ST_FETCH;
                        end
                    end
                    ST_BRANCH_CROSS: begin
                        pc_q <= target_q;
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_BIT_BRANCH_READ: begin
                        data_q <= data_in;
                        state_q <= ST_BIT_BRANCH_REPEAT;
                    end
                    ST_BIT_BRANCH_REPEAT: state_q <= ST_BIT_BRANCH_OFFSET;
                    ST_BIT_BRANCH_OFFSET: begin
                        pc_q <= pc_q + 16'd1;
                        target_q <= relative_target;
                        page_cross_q <= relative_base[15:8] != relative_target[15:8];
                        if (((op_q == OP_BBR) && !data_q[ir_q[6:4]]) ||
                            ((op_q == OP_BBS) &&  data_q[ir_q[6:4]])) begin
                            state_q <= ST_BIT_BRANCH_DUMMY;
                        end else begin
                            instruction_done <= 1'b1;
                            state_q <= ST_FETCH;
                        end
                    end
                    ST_BIT_BRANCH_DUMMY: begin
                        if (page_cross_q)
                            state_q <= ST_BIT_BRANCH_CROSS;
                        else begin
                            pc_q <= target_q;
                            instruction_done <= 1'b1;
                            state_q <= ST_FETCH;
                        end
                    end
                    ST_BIT_BRANCH_CROSS: begin
                        pc_q <= target_q;
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_PUSH_DUMMY: state_q <= ST_PUSH_WRITE;
                    ST_PUSH_WRITE: begin
                        s_q <= s_q - 8'd1;
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_PULL_DUMMY_PC: state_q <= ST_PULL_DUMMY_STACK;
                    ST_PULL_DUMMY_STACK: begin
                        s_q <= s_q + 8'd1;
                        state_q <= ST_PULL_READ;
                    end
                    ST_PULL_READ: begin
                        case (op_q)
                            OP_PLP: p_q <= normalize_p(data_in) & 8'hEF;
                            OP_PLA: begin a_q <= data_in; p_q <= with_nz(p_q, data_in); end
                            OP_PLX: begin x_q <= data_in; p_q <= with_nz(p_q, data_in); end
                            OP_PLY: begin y_q <= data_in; p_q <= with_nz(p_q, data_in); end
                            default: begin end
                        endcase
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_JSR_LOW: begin
                        lo_q <= data_in;
                        pc_q <= pc_q + 16'd1;
                        state_q <= ST_JSR_STACK_DUMMY;
                    end
                    ST_JSR_STACK_DUMMY: state_q <= ST_JSR_PUSH_HI;
                    ST_JSR_PUSH_HI: begin s_q <= s_q - 8'd1; state_q <= ST_JSR_PUSH_LO; end
                    ST_JSR_PUSH_LO: begin s_q <= s_q - 8'd1; state_q <= ST_JSR_HIGH; end
                    ST_JSR_HIGH: begin
                        pc_q <= {data_in, lo_q};
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_RTS_DUMMY_PC: state_q <= ST_RTS_DUMMY_STACK;
                    ST_RTS_DUMMY_STACK: begin s_q <= s_q + 8'd1; state_q <= ST_RTS_PULL_LO; end
                    ST_RTS_PULL_LO: begin lo_q <= data_in; s_q <= s_q + 8'd1; state_q <= ST_RTS_PULL_HI; end
                    ST_RTS_PULL_HI: begin pc_q <= {data_in, lo_q}; state_q <= ST_RTS_FINAL; end
                    ST_RTS_FINAL: begin
                        pc_q <= pc_q + 16'd1;
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_RTI_DUMMY_PC: state_q <= ST_RTI_DUMMY_STACK;
                    ST_RTI_DUMMY_STACK: begin s_q <= s_q + 8'd1; state_q <= ST_RTI_PULL_P; end
                    ST_RTI_PULL_P: begin p_q <= normalize_p(data_in) & 8'hEF; s_q <= s_q + 8'd1; state_q <= ST_RTI_PULL_LO; end
                    ST_RTI_PULL_LO: begin lo_q <= data_in; s_q <= s_q + 8'd1; state_q <= ST_RTI_PULL_HI; end
                    ST_RTI_PULL_HI: begin
                        pc_q <= {data_in, lo_q};
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_BRK_SIGNATURE: begin
                        pc_q <= pc_q + 16'd1;
                        op_q <= OP_BRK;
                        state_q <= ST_INT_PUSH_HI;
                    end
                    ST_INT_DUMMY: state_q <= ST_INT_PUSH_HI;
                    ST_INT_PUSH_HI: begin s_q <= s_q - 8'd1; state_q <= ST_INT_PUSH_LO; end
                    ST_INT_PUSH_LO: begin s_q <= s_q - 8'd1; state_q <= ST_INT_PUSH_P; end
                    ST_INT_PUSH_P: begin
                        s_q <= s_q - 8'd1;
                        p_q[P_I] <= 1'b1;
                        p_q[P_D] <= 1'b0;
                        state_q <= ST_INT_VECTOR_LO;
                    end
                    ST_INT_VECTOR_LO: begin lo_q <= data_in; state_q <= ST_INT_VECTOR_HI; end
                    ST_INT_VECTOR_HI: begin
                        pc_q <= {data_in, lo_q};
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_JMP_IND_LO: begin lo_q <= data_in; state_q <= ST_JMP_IND_HI; end
                    ST_JMP_IND_HI: begin hi_q <= data_in; state_q <= ST_JMP_IND_LAST; end
                    ST_JMP_IND_LAST: begin
                        pc_q <= {data_in, lo_q};
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_JMP_X_DUMMY: state_q <= ST_JMP_X_LO;
                    ST_JMP_X_LO: begin lo_q <= data_in; state_q <= ST_JMP_X_HI; end
                    ST_JMP_X_HI: begin
                        pc_q <= {data_in, lo_q};
                        instruction_done <= 1'b1;
                        state_q <= ST_FETCH;
                    end

                    ST_WAI_DUMMY: state_q <= ST_WAIT;
                    ST_WAIT: begin
                        if (nmi_pending_q || nmi_edge || !irq_n) begin
                            if (nmi_pending_q || nmi_edge || !p_q[P_I]) begin
                                vector_q <= (nmi_pending_q || nmi_edge)
                                          ? 16'hFFFA : 16'hFFFE;
                                nmi_pending_q <= 1'b0;
                                state_q <= ST_INT_DUMMY;
                            end else begin
                                state_q <= ST_FETCH;
                            end
                        end
                    end

                    ST_STP_DUMMY: state_q <= ST_STOP;
                    ST_STOP: state_q <= ST_STOP;

                    default: state_q <= ST_RESET_0;
                endcase
            end
        end
    end
endmodule
