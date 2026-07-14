`timescale 1ns / 1ps

module psram_driver (
    input  logic        clk,
    input  logic        resetn,

    // Ready/valid command interface.
    //   valid: master asserts when cmd/addr/wdata is presented.
    //   ready: driver asserts when it can accept a new command.
    //   Transfer occurs on the posedge where both valid and ready are high;
    //   cmd/addr/wdata are sampled into internal registers that cycle and
    //   may then change.
    input  logic        valid,
    output logic        ready,
    input  logic [7:0]  cmd,
    input  logic [23:0] addr,
    input  logic [63:0] wdata,

    // Response. rvalid pulses high for one cycle when rdata is meaningful
    // for the most recently accepted command; master should sample rdata
    // that cycle. For commands without read data, rvalid still pulses to
    // signal completion and rdata is don't-care.
    output logic        rvalid,
    output logic [63:0] rdata,
    output logic        done,

    // Capture tuning
    input  logic        dcount_wr_en,
    input  logic [4:0]  dcount_wr,
    input  logic        dcount_edge,

    // spi pins
    output logic [3:0] psram_oe,
    input  logic [3:0]  psram_a_i,
    output logic [3:0] psram_a_o,
    input  logic [3:0]  psram_b_i,
    output logic [3:0] psram_b_o,
    output logic      psram_ce_n,
    output logic       psram_clk
);

    logic is_quad;
    logic [31:0] sh_ce_n_tape;
    logic [31:0] sh_clken_tape;
    logic [31:0] sh_oe_tape;
    logic [31:0] sh_rd_en_tape;
    logic [63:0] sh_wr_a_tape; // includes cmd+address
    logic [63:0] sh_wr_b_tape;

    logic phy_active;
    
    assign phy_active = sh_clken_tape[31];

    // ------------------------------------------------------------------------
    // Forwarded clock
    // ------------------------------------------------------------------------
    logic psram_clk_oddr;

    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("ASYNC")
    ) psram_clk_oddr_i (
        .Q (psram_clk_oddr),
        .C (clk),
        .CE(1),
        .D1(1'b1),
        .D2(1'b0),
        .R (~phy_active),
        .S (1'b0)
    );

    localparam IDLE = 1'b0;
    localparam BUSY = 1'b1;

    logic state = IDLE;

    logic start_pulse;
    logic prev_ce_n;
    logic [2:0] ce_rest_cycles;
    logic [63:0] read_shift_q;
    logic launch_quad_q;

    // Registered command captured on the cycle of valid && ready. The
    // master is then free to change the inputs.
    logic [7:0]  cmd_q;
    logic [23:0] addr_q;
    logic [63:0] wdata_q;

    localparam ENTER_QPI    = 8'h35; // CMD_S template
    localparam RESET_ENABLE = 8'h66; // CMD_Q template
    localparam RESET        = 8'h99; // CMD_Q template
    localparam TOGGLE_WRAP  = 8'hC0; // CMD_Q template
    // In QPI mode the Lyontek write command is 0x02; 0x38 is the SPI-mode
    // quad-write variant.  Using 0x38 on the active QPI path can leave reads
    // working while writes fail to stick.
    localparam QPI_WRITE    = 8'h02; // QPIW_QUAD template
    localparam QPI_READ     = 8'hEB; // QPIR_QUAD template

    localparam CMD_S_QUAD = 1'b0;
    localparam CMD_Q_QUAD = 1'b1;
    localparam QPIW_QUAD  = 1'b1;
    localparam QPIR_QUAD  = 1'b1;

    // Op-completion detection. The "operation finished its useful work"
    // moment is *not* the CE-high transition — that's just the chip's
    // tCEH boundary that gates the next command. The actual data-valid
    // moment for a read is when RD_EN drops (read_shift_q is fully
    // populated); for writes/cmds it's when CLKEN drops (no more SCK
    // edges → the slave has latched everything we sent). Pulsing rvalid
    // at op-done, and gating `ready` separately on the CE-high tail,
    // decouples per-op latency from back-to-back throttling.
    logic prev_clken_q;
    logic prev_rd_en_q;
    wire  rd_en_drop = prev_rd_en_q && !sh_rd_en_tape[31];
    wire  clken_drop = prev_clken_q && !sh_clken_tape[31];
    wire  op_done    = (state == BUSY) &&
                       ((cmd_q == QPI_READ) ? rd_en_drop : clken_drop);

    // Gate ready on !rvalid: the cycle rvalid pulses, the master may still
    // be holding valid (it sees rvalid that cycle and drops valid the next),
    // and we must not re-accept the stale valid as a new command.
    // ce_rest_cycles holds ready low while the CE-high tail is in flight.
    assign ready = (state == IDLE) && (ce_rest_cycles == 0) && !rvalid;

    always @(posedge clk) begin
        if (!resetn) begin
            state <= IDLE;
            start_pulse <= 0;
            prev_ce_n <= 1;
            ce_rest_cycles <= 0;
            prev_clken_q <= 1'b0;
            prev_rd_en_q <= 1'b0;
            rvalid <= 1'b0;
            rdata   <= 64'd0;
            cmd_q   <= 8'd0;
            addr_q  <= 24'd0;
            wdata_q <= 64'd0;
            launch_quad_q <= 1'b0;
        end else begin
            start_pulse <= 0;
            rvalid      <= 0;
            prev_ce_n   <= sh_ce_n_tape[31];
            prev_clken_q <= sh_clken_tape[31];
            prev_rd_en_q <= sh_rd_en_tape[31];
            if (!prev_ce_n && sh_ce_n_tape[31]) begin
                // 50 ns (7 cycles rounded up) must pass
                // between assertions of psram_ce_n.
                // Detecting ce_rest_cycles has reached 0
                // is a cycle.  Generating start_pulse is
                // a cycle.  Setting up the tapes is a cycle.
                // so we only need a rest count of 4 to
                // yield a total rest interval of 7 cycles.
                ce_rest_cycles <= 3'h4;
            end else begin
                if (ce_rest_cycles != 0) begin
                    ce_rest_cycles <= ce_rest_cycles - 1;
                end
            end
            case (state)
            IDLE: begin
                if (valid && ready) begin
                    cmd_q       <= cmd;
                    addr_q      <= addr;
                    wdata_q     <= wdata;
                    start_pulse <= 1;
                    state       <= BUSY;
                    case (cmd)
                        ENTER_QPI:     launch_quad_q <= CMD_S_QUAD;
                        RESET_ENABLE,
                        RESET,
                        TOGGLE_WRAP:   launch_quad_q <= CMD_Q_QUAD;
                        QPI_WRITE:     launch_quad_q <= QPIW_QUAD;
                        QPI_READ:      launch_quad_q <= QPIR_QUAD;
                        default:       launch_quad_q <= is_quad;
                    endcase
                end
            end
            BUSY: begin
                // Pulse rvalid the cycle the operation's useful work
                // finishes. For QPI_READ that's the cycle RD_EN drops:
                // read_shift_q took its last sample the previous cycle
                // and now holds the full 64-bit line. For everything
                // else it's the cycle CLKEN drops. State returns to
                // IDLE here; ready is still gated by ce_rest_cycles
                // until the chip's tCEH has been honored.
                if (op_done) begin
                    rvalid <= 1'b1;
                    if (cmd_q == QPI_READ)
                        rdata <= read_shift_q;
                    state <= IDLE;
                end
            end
            endcase
        end
    end

    always_comb begin
        // engine-idle: high when no clock or read-enable bits remain queued
        done = (sh_clken_tape[31] == 0) && (sh_rd_en_tape[31] == 0);
        psram_clk = psram_clk_oddr;
    end

    // on cycles when OE is 0, OE[3:0] = 4'b0
    // on cycles when OE is 1, if QUAD is 0, OE[3:0] = {3'b0, 1'b1}, and 1 bit of wr_shift_a/_b is consumed
    // on cycles when OE is 1, if QUAD is 1, OE[3:0] = {4'b1}
    // on cycles when RD_EN is 1, rd_shift = {rd_shift_a/b[59:0], a/b_samp[3:0]} (always quad)

    // tape for SPI general commands
    localparam [31:0] CMD_S_CE_N_TAPE  = {{ 9{1'b0}}, {23{1'b1}} }; // CE_N (inverted) launches on next negedge
    localparam [31:0] CMD_S_CLKEN_TAPE = {{ 8{1'b1}}, {24{1'b0}} }; // SCK starts/stops on next posedge
    localparam [31:0] CMD_S_OE_TAPE    = {{ 8{1'b1}}, {24{1'b0}} }; // all output lines launch on next negedge       
    localparam [31:0] CMD_S_RD_EN_TAPE = {{32{1'b0}}};           // read is sampled on next posedge

    // tape for QPI general commands
    localparam [31:0] CMD_Q_CE_N_TAPE  = {{ 3{1'b0}}, {28{1'b1}}};
    localparam [31:0] CMD_Q_CLKEN_TAPE = {{ 2{1'b1}}, {30{1'b0}}};
    localparam [31:0] CMD_Q_OE_TAPE    = {{ 2{1'b1}}, {30{1'b0}}};
    localparam [31:0] CMD_Q_RD_EN_TAPE = {{32{1'b0}}};

    // tape for QPI write
    localparam [31:0] QPIW_CE_N_TAPE   = {{17{1'b0}}, {15{1'b1}}};
    localparam [31:0] QPIW_CLKEN_TAPE  = {{16{1'b1}}, {16{1'b0}}};
    localparam [31:0] QPIW_OE_TAPE     = {{16{1'b1}}, {16{1'b0}}};
    localparam [31:0] QPIW_RD_EN_TAPE  = {{32{1'b0}}};

    // tape for QPI read
    localparam [31:0] QPIR_CE_N_TAPE   = {{24{1'b0}}, {7{1'b1}}}; // 2 extra cycles of CE at the end
    localparam [31:0] QPIR_CLKEN_TAPE  = {{22{1'b1}}, {10{1'b0}}}; // 22 active cycles of events
    localparam [31:0] QPIR_OE_TAPE     = {{ 8{1'b1}}, {24{1'b0}}};
    // The eight-sample capture window starts after 16 idle tape slots to align
    // with valid PSRAM data on the positive IDDR phase.
    localparam [31:0] QPIR_RD_EN_TAPE  = {{16{1'b0}}, { 8{1'b1}}, { 8{1'b0}}};

    // ------------------------------------------------------------------------
    // Capture path: IDELAYE2 + IDDR
    // ------------------------------------------------------------------------

    logic [3:0] a_dly1, a_dly2;
    logic [3:0] b_dly1, b_dly2;
    logic [3:0] a_pos, a_neg;
    logic [3:0] b_pos, b_neg;
    logic [4:0] a_cntvalue [3:0];
    logic [4:0] b_cntvalue [3:0];

    wire [3:0] a_samp = dcount_edge ? a_neg : a_pos;
    wire [3:0] b_samp = dcount_edge ? b_neg : b_pos;
    wire [63:0] read_word_quad = {{read_shift_q[59:32], b_samp}, {read_shift_q[27:0], a_samp}};
    wire [63:0] read_word_spi  = {{read_shift_q[62:32], b_samp[1]}, {read_shift_q[30:0], a_samp[1]}};
    wire [63:0] read_word_next = is_quad ? read_word_quad : read_word_spi;


    // note that we aren't setting any ADDITIONAL delay on the IDELAYE2
    // blocks, but they have an inherent insertion time of about 600ns
    // which is just about right to be near the center of the data eye
    // at 133MHz.
    genvar gi;
    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : g_phy
            IDELAYE2 #(
                .CINVCTRL_SEL("FALSE"),
                .DELAY_SRC("IDATAIN"),
                .HIGH_PERFORMANCE_MODE("TRUE"),
                .IDELAY_TYPE("VAR_LOAD"),
                .IDELAY_VALUE(0),
                .PIPE_SEL("FALSE"),
                .REFCLK_FREQUENCY(200.0),
                .SIGNAL_PATTERN("DATA")
            ) u_a_idelay1 (
                .CNTVALUEOUT(a_cntvalue[gi]),
                .DATAOUT    (a_dly1[gi]),
                .C          (clk),
                .CE         (1'b0),
                .CINVCTRL   (1'b0),
                .CNTVALUEIN (dcount_wr),
                .DATAIN     (1'b0),
                .IDATAIN    (psram_a_i[gi]),
                .INC        (1'b0),
                .LD         (dcount_wr_en),
                .LDPIPEEN   (1'b0),
                .REGRST     (~resetn)
            );

            IDELAYE2 #(
                .CINVCTRL_SEL("FALSE"),
                .DELAY_SRC("IDATAIN"),
                .HIGH_PERFORMANCE_MODE("TRUE"),
                .IDELAY_TYPE("VAR_LOAD"),
                .IDELAY_VALUE(0),
                .PIPE_SEL("FALSE"),
                .REFCLK_FREQUENCY(200.0),
                .SIGNAL_PATTERN("DATA")
            ) u_b_idelay1 (
                .CNTVALUEOUT(b_cntvalue[gi]),
                .DATAOUT    (b_dly1[gi]),
                .C          (clk),
                .CE         (1'b0),
                .CINVCTRL   (1'b0),
                .CNTVALUEIN (dcount_wr),
                .DATAIN     (1'b0),
                .IDATAIN    (psram_b_i[gi]),
                .INC        (1'b0),
                .LD         (dcount_wr_en),
                .LDPIPEEN   (1'b0),
                .REGRST     (~resetn)
            );

            IDDR #(
                .DDR_CLK_EDGE("SAME_EDGE"),
                .INIT_Q1(1'b0),
                .INIT_Q2(1'b0),
                .SRTYPE("ASYNC")
            ) u_a_iddr (
                .Q1(a_pos[gi]),
                .Q2(a_neg[gi]),
                .C (clk),
                .CE(1'b1),
                .D (a_dly1[gi]),
                .R (~resetn),
                .S (1'b0)
            );

            IDDR #(
                .DDR_CLK_EDGE("SAME_EDGE"),
                .INIT_Q1(1'b0),
                .INIT_Q2(1'b0),
                .SRTYPE("ASYNC")
            ) u_b_iddr (
                .Q1(b_pos[gi]),
                .Q2(b_neg[gi]),
                .C (clk),
                .CE(1'b1),
                .D (b_dly1[gi]),
                .R (~resetn),
                .S (1'b0)
            );
        end
    endgenerate

    // ------------------------------------------------------------------------
    // Falling edge: launch outputs
    //
    // psram_ce_n keeps its synchronous reset (must be deasserted/high out of
    // reset). psram_oe / psram_a_o / psram_b_o intentionally have no reset:
    // the posedge FSM holds the sh_*_tape registers cleared during reset, so
    // these flops naturally launch zeros once the clock starts running. This
    // avoids a high-fanout (~2800 loads) reset broadcast through the OLOGIC
    // R pins on a half-cycle (negedge) launch path.
    // ------------------------------------------------------------------------
    always @(negedge clk) begin
        if (!resetn) begin
            psram_ce_n <= 1;
        end else begin
            psram_ce_n <= sh_ce_n_tape[31];
        end
    end

    always @(negedge clk) begin
        if (launch_quad_q) begin
            psram_oe <= {4{sh_oe_tape[31]}};
            psram_a_o <= sh_wr_a_tape[63:60];
            psram_b_o <= sh_wr_b_tape[63:60];
        end else begin
            psram_oe <= {3'b0, sh_oe_tape[31]};
            psram_a_o <= {3'b0, sh_wr_a_tape[63]};
            psram_b_o <= {3'b0, sh_wr_b_tape[63]};
        end
    end

    // ------------------------------------------------------------------------
    // Main engine
    // ------------------------------------------------------------------------
    always @(posedge clk) begin
        if (!resetn) begin
            is_quad <= 0;
            sh_ce_n_tape <= -1;
            sh_clken_tape <= 0;
            sh_oe_tape <= 0;
            sh_rd_en_tape <= 0;
            sh_wr_a_tape <= 0;
            sh_wr_b_tape <= 0;
            read_shift_q <= 64'd0;
            // (rdata is owned by the FSM always_ff above — its reset
            // and update both live there to avoid a multi-driver.)
        end else begin
            sh_ce_n_tape <= {sh_ce_n_tape[30:0],1'b1};
            sh_clken_tape <= {sh_clken_tape[30:0],1'b0};
            sh_oe_tape <= {sh_oe_tape[30:0],1'b0};
            sh_rd_en_tape <= {sh_rd_en_tape[30:0],1'b0};
            if (sh_oe_tape[31]) begin
                // if oe is enabled this cycle, shift the
                // wr tapes by 1/4 bits based on is_quad
                if (is_quad) begin
                    sh_wr_a_tape <= {sh_wr_a_tape[59:0],4'b0};
                    sh_wr_b_tape <= {sh_wr_b_tape[59:0],4'b0};
                end else begin
                    sh_wr_a_tape <= {sh_wr_a_tape[62:0],1'b0};
                    sh_wr_b_tape <= {sh_wr_b_tape[62:0],1'b0};
                end
            end
            if (sh_rd_en_tape[31]) begin
                read_shift_q <= read_word_next;
            end
            // (rdata is captured into the output register at op_done;
            // see the BUSY case below.)
            if (start_pulse) begin
                if (cmd_q == QPI_READ) begin
                    read_shift_q <= 64'd0;
                end
                case (cmd_q)
                    ENTER_QPI: begin // CMD_S template
                        is_quad <= CMD_S_QUAD;
                        sh_ce_n_tape <= CMD_S_CE_N_TAPE;
                        sh_clken_tape <= CMD_S_CLKEN_TAPE;
                        sh_oe_tape <= CMD_S_OE_TAPE;
                        sh_rd_en_tape <= CMD_S_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, 56'b0};
                        sh_wr_b_tape <= {cmd_q, 56'b0};
                    end
                    RESET_ENABLE: begin // CMD_Q template
                        is_quad <= CMD_Q_QUAD;
                        sh_ce_n_tape <= CMD_Q_CE_N_TAPE;
                        sh_clken_tape <= CMD_Q_CLKEN_TAPE;
                        sh_oe_tape <= CMD_Q_OE_TAPE;
                        sh_rd_en_tape <= CMD_Q_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, 56'b0};
                        sh_wr_b_tape <= {cmd_q, 56'b0};
                    end
                    RESET: begin // CMD_Q template
                        is_quad <= CMD_Q_QUAD;
                        sh_ce_n_tape <= CMD_Q_CE_N_TAPE;
                        sh_clken_tape <= CMD_Q_CLKEN_TAPE;
                        sh_oe_tape <= CMD_Q_OE_TAPE;
                        sh_rd_en_tape <= CMD_Q_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, 56'b0};
                        sh_wr_b_tape <= {cmd_q, 56'b0};
                    end
                    TOGGLE_WRAP: begin // CMD_Q template
                        is_quad <= CMD_Q_QUAD;
                        sh_ce_n_tape <= CMD_Q_CE_N_TAPE;
                        sh_clken_tape <= CMD_Q_CLKEN_TAPE;
                        sh_oe_tape <= CMD_Q_OE_TAPE;
                        sh_rd_en_tape <= CMD_Q_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, 56'b0};
                        sh_wr_b_tape <= {cmd_q, 56'b0};
                    end
                    QPI_WRITE: begin // QPIW_QUAD template
                        is_quad <= QPIW_QUAD;
                        sh_ce_n_tape <= QPIW_CE_N_TAPE;
                        sh_clken_tape <= QPIW_CLKEN_TAPE;
                        sh_oe_tape <= QPIW_OE_TAPE;
                        sh_rd_en_tape <= QPIW_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, addr_q, wdata_q[31:0]};
                        sh_wr_b_tape <= {cmd_q, addr_q, wdata_q[63:32]};
                    end
                    QPI_READ: begin // QPIR_QUAD template
                        is_quad <= QPIR_QUAD;
                        sh_ce_n_tape <= QPIR_CE_N_TAPE;
                        sh_clken_tape <= QPIR_CLKEN_TAPE;
                        sh_oe_tape <= QPIR_OE_TAPE;
                        sh_rd_en_tape <= QPIR_RD_EN_TAPE;
                        sh_wr_a_tape <= {cmd_q, addr_q, 32'b0};
                        sh_wr_b_tape <= {cmd_q, addr_q, 32'b0};
                    end
                    default: ; // do nothing on other commands
                endcase
            end
        end
    end

endmodule
