//
// Copyright (c) MikeJ - Jan 2005
// Copyright (c) 2016-2018 Sorgelig
//
// All rights reserved
//
// Redistribution and use in source and synthesized forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// Redistributions in synthesized form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// Neither the name of the author nor the names of other contributors may
// be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS CODE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Source: a2fpga_core hdl/support/YM2149.sv
// https://github.com/a2fpga/a2fpga_core/blob/20291afa7f7f9e438615e2cbe1d8e29de5e1f835/hdl/support/YM2149.sv
//

// BDIR  BC  MODE
//   0   0   inactive
//   0   1   read value
//   1   0   write value
//   1   1   set address
//

module YM2149
(
    input        CLK,       // Global clock
    input        CE,        // PSG Clock enable
    input        RESET,     // Chip RESET (set all Registers to '0', active hi)
    input        BDIR,      // Bus Direction (0 - read , 1 - write)
    input        BC,        // Bus control
    input  [7:0] DI,        // Data In
    output [7:0] DO,        // Data Out
    output [7:0] CHANNEL_A, // PSG Output channel A
    output [7:0] CHANNEL_B, // PSG Output channel B
    output [7:0] CHANNEL_C, // PSG Output channel C

    input        SEL,
    input        MODE,

    output [5:0] ACTIVE,

    input  [7:0] IOA_in,
    output [7:0] IOA_out,

    input  [7:0] IOB_in,
    output [7:0] IOB_out
);

reg [7:0] ymreg[16];

assign ACTIVE  = ~ymreg[7][5:0];
assign IOA_out = ymreg[14];
assign IOB_out = ymreg[15];

reg address_latched;
reg [7:0] addr;

// Write to PSG
reg env_reset;
reg noise_reset;
always @(posedge CLK) begin
    if (RESET) begin
        ymreg <= '{default:0};
        addr <= '0;
        env_reset <= 1'b0;
        noise_reset <= 1'b0;
        address_latched <= 1'b0;
    end else begin
        env_reset <= 1'b0;
        noise_reset <= 1'b0;
        if (BDIR) begin
            if (BC) begin
                addr <= DI;
                address_latched <= 1'b1;
            end else if (address_latched & !addr[7:4]) begin
                ymreg[addr[3:0]] <= DI;
                noise_reset <= (addr == 8'd6);
                env_reset <= (addr == 8'd13);
            end
        end
    end
end

// Read from PSG
reg [7:0] dout;
assign DO = dout;
always_comb begin
    dout = 8'hFF;
    if (address_latched & ~BDIR & BC & !addr[7:4]) begin
        case (addr[3:0])
            4'd0:  dout = ymreg[0];
            4'd1:  dout = ymreg[1][3:0];
            4'd2:  dout = ymreg[2];
            4'd3:  dout = ymreg[3][3:0];
            4'd4:  dout = ymreg[4];
            4'd5:  dout = ymreg[5][3:0];
            4'd6:  dout = ymreg[6][4:0];
            4'd7:  dout = ymreg[7];
            4'd8:  dout = ymreg[8][4:0];
            4'd9:  dout = ymreg[9][4:0];
            4'd10: dout = ymreg[10][4:0];
            4'd11: dout = ymreg[11];
            4'd12: dout = ymreg[12];
            4'd13: dout = ymreg[13][3:0];
            // AY-3-8913 has no I/O ports; regs 14/15 act as plain
            // last-written latches. Match AppleWin's AY8913 readback,
            // which returns the stored value regardless of the
            // mixer's IOA/IOB direction bits (which are no-ops on a
            // real AY-3-8913).
            4'd14: dout = ymreg[14];
            4'd15: dout = ymreg[15];
        endcase
    end
end

reg ena_div;
reg ena_div_noise;

// p_divider
always @(posedge CLK) begin
    reg [3:0] cnt_div;
    reg       noise_div;

    if (RESET) begin
        cnt_div <= 4'd0;
        noise_div <= 1'b0;
        ena_div <= 1'b0;
        ena_div_noise <= 1'b0;
    end else if (CE) begin
        ena_div <= 1'b0;
        ena_div_noise <= 1'b0;
        if (!cnt_div) begin
            cnt_div <= {SEL, 3'b111};
            ena_div <= 1'b1;

            noise_div <= ~noise_div;
            if (noise_div)
                ena_div_noise <= 1'b1;
        end else begin
            cnt_div <= cnt_div - 1'b1;
        end
    end
end

reg [2:0] noise_gen_op;
wire [4:0] noise_period = ymreg[6][4:0] ? ymreg[6][4:0] : 5'd1;

// p_noise_gen
always @(posedge CLK) begin
    reg [16:0] poly17;
    reg [4:0]  noise_gen_cnt;
    reg        noise_toggle;

    if (RESET) begin
        poly17 <= 17'h00001;
        noise_gen_cnt <= 5'd0;
        noise_toggle <= 1'b0;
        noise_gen_op <= 3'b111;
    end else if (noise_reset) begin
        noise_gen_cnt <= 5'd0;
    end else if (CE) begin
        if (ena_div_noise) begin
            if (noise_gen_cnt >= noise_period - 1'd1) begin
                noise_gen_cnt <= 5'd0;
                if (poly17[0] ^ poly17[1])
                    noise_toggle <= ~noise_toggle;
                poly17 <= {(poly17[0] ^ poly17[2]), poly17[16:1]};
            end else begin
                noise_gen_cnt <= noise_gen_cnt + 1'd1;
            end
            noise_gen_op <= {3{~noise_toggle}};
        end
    end
end

wire [11:0] tone_gen_freq[1:3];
assign tone_gen_freq[1] = {ymreg[1][3:0], ymreg[0]};
assign tone_gen_freq[2] = {ymreg[3][3:0], ymreg[2]};
assign tone_gen_freq[3] = {ymreg[5][3:0], ymreg[4]};

reg [3:1] tone_gen_op;

// p_tone_gens
always @(posedge CLK) begin
    integer i;
    reg [11:0] tone_gen_cnt[1:3];

    if (CE) begin
        for (i = 1; i <= 3; i = i + 1) begin
            if (ena_div) begin
                if (tone_gen_freq[i]) begin
                    if (tone_gen_cnt[i] >= (tone_gen_freq[i] - 1'd1)) begin
                        tone_gen_cnt[i] <= 12'd0;
                        tone_gen_op[i] <= ~tone_gen_op[i];
                    end else begin
                        tone_gen_cnt[i] <= tone_gen_cnt[i] + 1'd1;
                    end
                end else begin
                    tone_gen_op[i] <= ymreg[7][i];
                    tone_gen_cnt[i] <= 12'd0;
                end
            end
        end
    end
end

reg env_ena;
wire [15:0] env_gen_comp = {ymreg[12], ymreg[11]} ? {ymreg[12], ymreg[11]} - 1'd1 : 16'd0;

// p_envelope_freq
always @(posedge CLK) begin
    reg [15:0] env_gen_cnt;

    if (RESET || env_reset) begin
        env_gen_cnt <= 16'd0;
        env_ena <= 1'b0;
    end else if (CE) begin
        env_ena <= 1'b0;
        if (ena_div) begin
            if (env_gen_cnt >= env_gen_comp) begin
                env_gen_cnt <= 16'd0;
                env_ena <= 1'b1;
            end else begin
                env_gen_cnt <= env_gen_cnt + 1'd1;
            end
        end
    end
end

reg [4:0] env_vol;

wire is_bot    = (env_vol == 5'b00000);
wire is_bot_p1 = (env_vol == 5'b00001);
wire is_top_m1 = (env_vol == 5'b11110);
wire is_top    = (env_vol == 5'b11111);

always @(posedge CLK) begin
    reg env_hold;
    reg env_inc;

    // envelope shapes
    // C AtAlH
    // 0 0 x x  \___
    //
    // 0 1 x x  /___
    //
    // 1 0 0 0  \\\\
    //
    // 1 0 0 1  \___
    //
    // 1 0 1 0  \/\/
    //           ___
    // 1 0 1 1  \
    //
    // 1 1 0 0  ////
    //           ___
    // 1 1 0 1  /
    //
    // 1 1 1 0  /\/\
    //
    // 1 1 1 1  /___

    if (env_reset | RESET) begin
        // load initial state
        if (!ymreg[13][2]) begin
            env_vol <= 5'b11111;
            env_inc <= 1'b0;
        end else begin
            env_vol <= 5'b00000;
            env_inc <= 1'b1;
        end
        env_hold <= 1'b0;
    end else if (CE) begin
        if (env_ena) begin
            if (!env_hold) begin
                if (env_inc)
                    env_vol <= env_vol + 5'b00001;
                else
                    env_vol <= env_vol + 5'b11111;
            end

            // envelope shape control.
            if (!ymreg[13][3]) begin
                if (!env_inc) begin
                    if (is_bot_p1)
                        env_hold <= 1'b1;
                end else if (is_top) begin
                    env_hold <= 1'b1;
                end
            end else if (ymreg[13][0]) begin
                if (!env_inc) begin
                    if (ymreg[13][1]) begin
                        if (is_bot)
                            env_hold <= 1'b1;
                    end else if (is_bot_p1) begin
                        env_hold <= 1'b1;
                    end
                end else if (ymreg[13][1]) begin
                    if (is_top)
                        env_hold <= 1'b1;
                end else if (is_top_m1) begin
                    env_hold <= 1'b1;
                end
            end else if (ymreg[13][1]) begin
                if (env_inc == 1'b0) begin
                    if (is_bot_p1)
                        env_hold <= 1'b1;
                    if (is_bot) begin
                        env_hold <= 1'b0;
                        env_inc <= 1'b1;
                    end
                end else begin
                    if (is_top_m1)
                        env_hold <= 1'b1;
                    if (is_top) begin
                        env_hold <= 1'b0;
                        env_inc <= 1'b0;
                    end
                end
            end
        end
    end
end

reg [5:0] A, B, C;
always @(posedge CLK) begin
    A <= {MODE, ~((ymreg[7][0] | tone_gen_op[1]) & (ymreg[7][3] | noise_gen_op[0])) ? 5'd0 : ymreg[8][4]  ? env_vol[4:0] : { ymreg[8][3:0],  ymreg[8][3]}};
    B <= {MODE, ~((ymreg[7][1] | tone_gen_op[2]) & (ymreg[7][4] | noise_gen_op[1])) ? 5'd0 : ymreg[9][4]  ? env_vol[4:0] : { ymreg[9][3:0],  ymreg[9][3]}};
    C <= {MODE, ~((ymreg[7][2] | tone_gen_op[3]) & (ymreg[7][5] | noise_gen_op[2])) ? 5'd0 : ymreg[10][4] ? env_vol[4:0] : {ymreg[10][3:0], ymreg[10][3]}};
end

reg [7:0] volTable[64];
initial begin
    // YM2149
    volTable[0]  = 8'h00;
    volTable[1]  = 8'h01;
    volTable[2]  = 8'h01;
    volTable[3]  = 8'h02;
    volTable[4]  = 8'h02;
    volTable[5]  = 8'h03;
    volTable[6]  = 8'h03;
    volTable[7]  = 8'h04;
    volTable[8]  = 8'h06;
    volTable[9]  = 8'h07;
    volTable[10] = 8'h09;
    volTable[11] = 8'h0a;
    volTable[12] = 8'h0c;
    volTable[13] = 8'h0e;
    volTable[14] = 8'h11;
    volTable[15] = 8'h13;
    volTable[16] = 8'h17;
    volTable[17] = 8'h1b;
    volTable[18] = 8'h20;
    volTable[19] = 8'h25;
    volTable[20] = 8'h2c;
    volTable[21] = 8'h35;
    volTable[22] = 8'h3e;
    volTable[23] = 8'h47;
    volTable[24] = 8'h54;
    volTable[25] = 8'h66;
    volTable[26] = 8'h77;
    volTable[27] = 8'h88;
    volTable[28] = 8'ha1;
    volTable[29] = 8'hc0;
    volTable[30] = 8'he0;
    volTable[31] = 8'hff;

    // AY8910
    volTable[32] = 8'h00;
    volTable[33] = 8'h00;
    volTable[34] = 8'h03;
    volTable[35] = 8'h03;
    volTable[36] = 8'h04;
    volTable[37] = 8'h04;
    volTable[38] = 8'h06;
    volTable[39] = 8'h06;
    volTable[40] = 8'h0a;
    volTable[41] = 8'h0a;
    volTable[42] = 8'h0f;
    volTable[43] = 8'h0f;
    volTable[44] = 8'h15;
    volTable[45] = 8'h15;
    volTable[46] = 8'h22;
    volTable[47] = 8'h22;
    volTable[48] = 8'h28;
    volTable[49] = 8'h28;
    volTable[50] = 8'h41;
    volTable[51] = 8'h41;
    volTable[52] = 8'h5b;
    volTable[53] = 8'h5b;
    volTable[54] = 8'h72;
    volTable[55] = 8'h72;
    volTable[56] = 8'h90;
    volTable[57] = 8'h90;
    volTable[58] = 8'hb5;
    volTable[59] = 8'hb5;
    volTable[60] = 8'hd7;
    volTable[61] = 8'hd7;
    volTable[62] = 8'hff;
    volTable[63] = 8'hff;
end

assign CHANNEL_A = volTable[A];
assign CHANNEL_B = volTable[B];
assign CHANNEL_C = volTable[C];

endmodule
