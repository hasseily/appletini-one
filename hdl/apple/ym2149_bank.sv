`timescale 1ns / 1ps

// N sound chips (YM2149 / AY-3-8910) that share one copy of the chip logic.
//
// Each chip has a slot. The module serves one slot per clock: it reads the
// state of that chip from LUT-RAM, applies one bus/clock event, and writes
// the state back one clock later.
//
// A chip changes state only on a PSG clock enable or on bus activity. Each
// chip has a small event queue that keeps the input samples of those clocks
// in order. A level that stays the same (a VIA holds BDIR for many cycles)
// has no more effect after two clocks, so the queue takes three samples of
// it and then stops. The result is the same state as YM2149.sv, some clocks
// later (at most 4 * N clocks after a burst).
//
// DO and the channel outputs are registers. They follow the chip state when
// its slot comes, each N clocks.
//
// BDIR  BC  MODE
//   0   0   inactive
//   0   1   read value
//   1   0   write value
//   1   1   set address
module ym2149_bank #(
    parameter int N = 4
) (
    input  logic         clk,
    input  logic [N-1:0] ce,      // PSG clock enable, one clock wide
    input  logic [N-1:0] reset,   // active high
    input  logic [N-1:0] bdir,
    input  logic [N-1:0] bc,
    input  logic [N-1:0] mode,    // 0 = YM2149 volume table, 1 = AY-3-8910
    input  logic [7:0]   di [N],
    output logic [7:0]   dout [N],
    output logic [7:0]   channel [N*3]
);

    localparam int SLOT_BITS = (N <= 2) ? 1 : $clog2(N);
    localparam int DEPTH     = 1 << SLOT_BITS;
    localparam int QUEUE_BITS = 5;

    typedef struct packed {
        logic       reset;
        logic       bdir;
        logic       bc;
        logic       ce;
        logic [7:0] di;
    } event_t;

    typedef struct packed {
        logic        address_latched;
        logic        addr_high;       // address bits 7:4 are not zero
        logic [3:0]  addr;
        logic        env_reset;
        logic        noise_reset;
        logic [3:0]  cnt_div;
        logic        noise_div;
        logic        ena_div;
        logic        ena_div_noise;
        logic [16:0] poly17;
        logic [4:0]  noise_gen_cnt;
        logic        noise_toggle;
        logic        noise_gen_op;
        logic [2:0][11:0] tone_gen_cnt;
        logic [2:0]  tone_gen_op;
        logic [15:0] env_gen_cnt;
        logic        env_ena;
        logic [4:0]  env_vol;
        logic        env_hold;
        logic        env_inc;
    } dyn_t;

    // ---- Event queues ----
    logic [SLOT_BITS-1:0] slot = '0;
    logic [SLOT_BITS-1:0] slot_q = '0;
    event_t               queue_head [N];
    logic [N-1:0]         queue_ready;

    generate
        for (genvar c = 0; c < N; c++) begin : g_queue
            (* ram_style = "distributed" *)
            logic [$bits(event_t)-1:0] queue_mem [0:(1 << QUEUE_BITS)-1];
            logic [QUEUE_BITS-1:0] write_q = '0;
            logic [QUEUE_BITS-1:0] read_q = '0;
            logic [10:0]           level_q = '0;
            logic [1:0]            settle_q = '0;

            // Data and BC have an effect on the state only with BDIR.
            wire [10:0] level = {reset[c], bdir[c], bdir[c] && bc[c],
                                 bdir[c] ? di[c] : 8'h00};
            wire        fresh = ce[c] || (level != level_q);
            wire        push  = fresh || (settle_q != 2'd0);
            wire        pop   = queue_ready[c] && (slot == SLOT_BITS'(c));

            assign queue_ready[c] = (write_q != read_q);
            assign queue_head[c]  = event_t'(queue_mem[read_q]);

            always_ff @(posedge clk) begin
                level_q <= level;
                if (fresh) begin
                    settle_q <= 2'd2;
                end else if (settle_q != 2'd0) begin
                    settle_q <= settle_q - 2'd1;
                end
                if (push) begin
                    queue_mem[write_q] <= {reset[c], bdir[c], bc[c], ce[c], di[c]};
                    write_q <= write_q + 1'b1;
                end
                if (pop) begin
                    read_q <= read_q + 1'b1;
                end
            end

`ifndef SYNTHESIS
            always_ff @(posedge clk) begin
                if (push && (QUEUE_BITS'(write_q + 1'b1) == read_q)) begin
                    $fatal(1, "ym2149_bank: event queue %0d is full", c);
                end
            end
`endif
        end
    endgenerate

    // ---- Stage 0: read the state and the event of this slot ----
    (* ram_style = "distributed" *) dyn_t dyn_mem [0:DEPTH-1];
    initial begin
        for (int i = 0; i < DEPTH; i++) dyn_mem[i] = '0;
    end

    dyn_t       s;
    event_t     ev;
    logic       ev_valid = 1'b0;
    logic [7:0] ymreg [16];
    logic [15:0] reg_we;
    logic [7:0]  reg_wdata;
    dyn_t        nx;

    always_ff @(posedge clk) begin
        slot     <= (slot == SLOT_BITS'(N - 1)) ? '0 : slot + 1'b1;
        slot_q   <= slot;
        s        <= dyn_mem[slot];
        ev       <= queue_head[slot];
        ev_valid <= queue_ready[slot];
        if (ev_valid) begin
            dyn_mem[slot_q] <= nx;
        end
    end

    // One small RAM for each chip register. Only the bits that the chip
    // uses are kept; the other bits read as zero, as in YM2149.sv.
    localparam int REG_W [16] = '{8, 4, 8, 4, 8, 4, 5, 8, 5, 5, 5, 8, 8, 4, 8, 8};
    generate
        for (genvar r = 0; r < 16; r++) begin : g_reg
            (* ram_style = "distributed" *)
            logic [REG_W[r]-1:0] mem [0:DEPTH-1];
            logic [REG_W[r]-1:0] read_q = '0;
            initial begin
                for (int i = 0; i < DEPTH; i++) mem[i] = '0;
            end
            always_ff @(posedge clk) begin
                if (ev_valid && reg_we[r]) begin
                    mem[slot_q] <= reg_wdata[REG_W[r]-1:0];
                end
                read_q <= mem[slot];
            end
            assign ymreg[r] = 8'(read_q);
        end
    endgenerate

    // ---- Stage 1: the chip logic, the same rules as YM2149.sv ----
    wire [4:0]  noise_period = (ymreg[6][4:0] != 5'd0) ? ymreg[6][4:0] : 5'd1;
    wire [15:0] env_period   = {ymreg[12], ymreg[11]};
    wire [15:0] env_gen_comp = (env_period != 16'd0) ? env_period - 1'd1 : 16'd0;
    wire        is_bot       = (s.env_vol == 5'b00000);
    wire        is_bot_p1    = (s.env_vol == 5'b00001);
    wire        is_top_m1    = (s.env_vol == 5'b11110);
    wire        is_top       = (s.env_vol == 5'b11111);

    logic [11:0] tone_gen_freq [3];
    always_comb begin
        tone_gen_freq[0] = {ymreg[1][3:0], ymreg[0]};
        tone_gen_freq[1] = {ymreg[3][3:0], ymreg[2]};
        tone_gen_freq[2] = {ymreg[5][3:0], ymreg[4]};
    end

    always_comb begin
        nx        = s;
        reg_we    = 16'h0000;
        reg_wdata = ev.di;

        // Bus write
        if (ev.reset) begin
            reg_we             = 16'hFFFF;
            reg_wdata          = 8'h00;
            nx.addr            = 4'd0;
            nx.addr_high       = 1'b0;
            nx.env_reset       = 1'b0;
            nx.noise_reset     = 1'b0;
            nx.address_latched = 1'b0;
        end else begin
            nx.env_reset   = 1'b0;
            nx.noise_reset = 1'b0;
            if (ev.bdir) begin
                if (ev.bc) begin
                    nx.addr            = ev.di[3:0];
                    nx.addr_high       = (ev.di[7:4] != 4'd0);
                    nx.address_latched = 1'b1;
                end else if (s.address_latched && !s.addr_high) begin
                    reg_we[s.addr]  = 1'b1;
                    nx.noise_reset  = (s.addr == 4'd6);
                    nx.env_reset    = (s.addr == 4'd13);
                end
            end
        end

        // Clock divider
        if (ev.reset) begin
            nx.cnt_div       = 4'd0;
            nx.noise_div     = 1'b0;
            nx.ena_div       = 1'b0;
            nx.ena_div_noise = 1'b0;
        end else if (ev.ce) begin
            nx.ena_div       = 1'b0;
            nx.ena_div_noise = 1'b0;
            if (s.cnt_div == 4'd0) begin
                nx.cnt_div   = 4'b0111;
                nx.ena_div   = 1'b1;
                nx.noise_div = ~s.noise_div;
                if (s.noise_div) nx.ena_div_noise = 1'b1;
            end else begin
                nx.cnt_div = s.cnt_div - 1'b1;
            end
        end

        // Noise generator
        if (ev.reset) begin
            nx.poly17        = 17'h00001;
            nx.noise_gen_cnt = 5'd0;
            nx.noise_toggle  = 1'b0;
            nx.noise_gen_op  = 1'b1;
        end else if (s.noise_reset) begin
            nx.noise_gen_cnt = 5'd0;
        end else if (ev.ce) begin
            if (s.ena_div_noise) begin
                if (s.noise_gen_cnt >= noise_period - 1'd1) begin
                    nx.noise_gen_cnt = 5'd0;
                    if (s.poly17[0] ^ s.poly17[1]) nx.noise_toggle = ~s.noise_toggle;
                    nx.poly17 = {(s.poly17[0] ^ s.poly17[2]), s.poly17[16:1]};
                end else begin
                    nx.noise_gen_cnt = s.noise_gen_cnt + 1'd1;
                end
                nx.noise_gen_op = ~s.noise_toggle;
            end
        end

        // Tone generators (no reset, as in YM2149.sv)
        if (ev.ce && s.ena_div) begin
            for (int i = 0; i < 3; i++) begin
                if (tone_gen_freq[i] != 12'd0) begin
                    if (s.tone_gen_cnt[i] >= (tone_gen_freq[i] - 1'd1)) begin
                        nx.tone_gen_cnt[i] = 12'd0;
                        nx.tone_gen_op[i]  = ~s.tone_gen_op[i];
                    end else begin
                        nx.tone_gen_cnt[i] = s.tone_gen_cnt[i] + 1'd1;
                    end
                end else begin
                    nx.tone_gen_op[i]  = ymreg[7][i + 1];
                    nx.tone_gen_cnt[i] = 12'd0;
                end
            end
        end

        // Envelope period
        if (ev.reset || s.env_reset) begin
            nx.env_gen_cnt = 16'd0;
            nx.env_ena     = 1'b0;
        end else if (ev.ce) begin
            nx.env_ena = 1'b0;
            if (s.ena_div) begin
                if (s.env_gen_cnt >= env_gen_comp) begin
                    nx.env_gen_cnt = 16'd0;
                    nx.env_ena     = 1'b1;
                end else begin
                    nx.env_gen_cnt = s.env_gen_cnt + 1'd1;
                end
            end
        end

        // Envelope shape
        if (s.env_reset || ev.reset) begin
            if (!ymreg[13][2]) begin
                nx.env_vol = 5'b11111;
                nx.env_inc = 1'b0;
            end else begin
                nx.env_vol = 5'b00000;
                nx.env_inc = 1'b1;
            end
            nx.env_hold = 1'b0;
        end else if (ev.ce && s.env_ena) begin
            if (!s.env_hold) begin
                nx.env_vol = s.env_inc ? s.env_vol + 5'b00001 : s.env_vol + 5'b11111;
            end
            if (!ymreg[13][3]) begin
                if (!s.env_inc) begin
                    if (is_bot_p1) nx.env_hold = 1'b1;
                end else if (is_top) begin
                    nx.env_hold = 1'b1;
                end
            end else if (ymreg[13][0]) begin
                if (!s.env_inc) begin
                    if (ymreg[13][1]) begin
                        if (is_bot) nx.env_hold = 1'b1;
                    end else if (is_bot_p1) begin
                        nx.env_hold = 1'b1;
                    end
                end else if (ymreg[13][1]) begin
                    if (is_top) nx.env_hold = 1'b1;
                end else if (is_top_m1) begin
                    nx.env_hold = 1'b1;
                end
            end else if (ymreg[13][1]) begin
                if (!s.env_inc) begin
                    if (is_bot_p1) nx.env_hold = 1'b1;
                    if (is_bot) begin
                        nx.env_hold = 1'b0;
                        nx.env_inc  = 1'b1;
                    end
                end else begin
                    if (is_top_m1) nx.env_hold = 1'b1;
                    if (is_top) begin
                        nx.env_hold = 1'b0;
                        nx.env_inc  = 1'b0;
                    end
                end
            end
        end
    end

    // ---- Outputs of the chip in stage 1 ----
    logic [7:0] vol_table [64];
    initial begin
        // YM2149
        vol_table[0]  = 8'h00; vol_table[1]  = 8'h01; vol_table[2]  = 8'h01; vol_table[3]  = 8'h02;
        vol_table[4]  = 8'h02; vol_table[5]  = 8'h03; vol_table[6]  = 8'h03; vol_table[7]  = 8'h04;
        vol_table[8]  = 8'h06; vol_table[9]  = 8'h07; vol_table[10] = 8'h09; vol_table[11] = 8'h0a;
        vol_table[12] = 8'h0c; vol_table[13] = 8'h0e; vol_table[14] = 8'h11; vol_table[15] = 8'h13;
        vol_table[16] = 8'h17; vol_table[17] = 8'h1b; vol_table[18] = 8'h20; vol_table[19] = 8'h25;
        vol_table[20] = 8'h2c; vol_table[21] = 8'h35; vol_table[22] = 8'h3e; vol_table[23] = 8'h47;
        vol_table[24] = 8'h54; vol_table[25] = 8'h66; vol_table[26] = 8'h77; vol_table[27] = 8'h88;
        vol_table[28] = 8'ha1; vol_table[29] = 8'hc0; vol_table[30] = 8'he0; vol_table[31] = 8'hff;
        // AY-3-8910
        vol_table[32] = 8'h00; vol_table[33] = 8'h00; vol_table[34] = 8'h03; vol_table[35] = 8'h03;
        vol_table[36] = 8'h04; vol_table[37] = 8'h04; vol_table[38] = 8'h06; vol_table[39] = 8'h06;
        vol_table[40] = 8'h0a; vol_table[41] = 8'h0a; vol_table[42] = 8'h0f; vol_table[43] = 8'h0f;
        vol_table[44] = 8'h15; vol_table[45] = 8'h15; vol_table[46] = 8'h22; vol_table[47] = 8'h22;
        vol_table[48] = 8'h28; vol_table[49] = 8'h28; vol_table[50] = 8'h41; vol_table[51] = 8'h41;
        vol_table[52] = 8'h5b; vol_table[53] = 8'h5b; vol_table[54] = 8'h72; vol_table[55] = 8'h72;
        vol_table[56] = 8'h90; vol_table[57] = 8'h90; vol_table[58] = 8'hb5; vol_table[59] = 8'hb5;
        vol_table[60] = 8'hd7; vol_table[61] = 8'hd7; vol_table[62] = 8'hff; vol_table[63] = 8'hff;
    end

    logic [5:0] level_index [3];
    always_comb begin
        for (int i = 0; i < 3; i++) begin
            logic [4:0] volume;
            volume = ymreg[8 + i][4:0];
            level_index[i][5] = mode[slot_q];
            if (!((ymreg[7][i] || s.tone_gen_op[i]) &&
                  (ymreg[7][3 + i] || s.noise_gen_op))) begin
                level_index[i][4:0] = 5'd0;
            end else if (volume[4]) begin
                level_index[i][4:0] = s.env_vol;
            end else begin
                level_index[i][4:0] = {volume[3:0], volume[3]};
            end
        end
    end

    logic [7:0]   read_value_q [N];
    logic [N-1:0] read_ready_q = '0;
    logic [7:0]   channel_q [N*3];

    always_ff @(posedge clk) begin
        for (int c = 0; c < N; c++) begin
            if (slot_q == SLOT_BITS'(c)) begin
                read_value_q[c] <= ymreg[s.addr];
                read_ready_q[c] <= s.address_latched && !s.addr_high;
                for (int i = 0; i < 3; i++) begin
                    channel_q[c*3 + i] <= vol_table[level_index[i]];
                end
            end
        end
    end

    always_comb begin
        for (int c = 0; c < N; c++) begin
            dout[c] = (read_ready_q[c] && !bdir[c] && bc[c]) ? read_value_q[c] : 8'hFF;
        end
        for (int i = 0; i < N*3; i++) begin
            channel[i] = channel_q[i];
        end
    end

endmodule
