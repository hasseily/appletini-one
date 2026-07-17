`timescale 1ns / 1ps

// Synetix SuperSprite clone -- PL front end for the PS software VDP.
//
// SuperSprite pairs a TI TMS9918A VDP with an AY-3-8910 PSG and a
// TMS5220 speech chip, plus an off-chip black-level video overlay. Appletini
// renders all video in the PS (CPU1 -> Apple FB -> compositor -> HDMI), so this
// module does NOT render pixels. Instead it implements only the part that must
// run in real time at the Apple bus: the TMS9918 *memory/register interface*.
// The PS reads VRAM + registers out of here and renders the VDP image in
// software, which the compositor overlays (black-key) onto the Apple picture.
//
// What lives here (real-time, bounded):
//   - Slot device-select decode ($C0nX, n = 8+slot; slot 7 -> $C0Fx). The
//     SuperSprite hardware and software are hard-wired to slot 7.
//   - TMS9918 command/data port protocol: 14-bit auto-increment VRAM address,
//     the two-byte address/register flip-flop, VRAM read-ahead buffer, the 8
//     write-only registers R0..R7, and the read-only status register.
//   - 16 KB VRAM in BRAM, dual-port (Apple bus side + PS read side).
//   - VBlank frame flag (status bit 7) + maskable interrupt (R1 bit 5).
//   - The SuperSprite video soft switches ($C0n3..$C0n6).
//
// What lives in the PS (see supersprite_vdp.c): tile/sprite rendering, the
// sprite-coincidence / fifth-sprite status bits, palette, and the overlay.
//
// Bus timing follows the mockingboard.sv / vidhd_card.sv idioms in this repo:
// reads are presented while ab_read.sss_en is high; write and read side-effect
// strobes fire on ab_read.data_en. The fabric clock is ~130x the Apple bus, so
// the 1-cycle BRAM read latency is hidden between accesses.

module supersprite_card (
    input  logic                    clk,
    input  logic                    rstn,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    input  logic [2:0]              slot_assign,   // 3'd7 when enabled, else 0

    // ~1-cycle pulse at the VDP frame rate (50/60 Hz), generated in apple_top.
    input  logic                    vblank_tick,

    // Sprite-status bits maintained by the PS renderer: {5S, C, fifth_num[4:0]}.
    input  logic [6:0]              ps_status_flags,

    output globals::AppleBus_write  ab_write,
    output logic                    irq_n,         // active-low VDP interrupt
    output logic signed [15:0]      ssp_audio,     // AY-3-8910 PSG mono audio

    // ---- PS-facing readback (software renderer) ----
    input  logic [13:0]             ps_vram_addr,
    output logic [7:0]              ps_vram_data,  // registered read of VRAM
    output logic [63:0]             ps_regs,       // R0 in [7:0] .. R7 in [63:56]
    output logic [7:0]              ps_status,     // live status byte
    output logic [15:0]             ps_frame,      // increments each vblank_tick
    output logic                    ps_status_read,// 1-cycle pulse on status read
    output logic                    ps_apple_video, // apple video switch
    output logic                    ps_vdp_overlay  // VDP overlay switch
);

    // ---- Slot device-select decode: $C0(8+slot)x, offsets 0..F ----
    wire card_enabled = (slot_assign != 3'd0);
    wire io_hit =
        card_enabled &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + slot_assign));
    wire [3:0] off = ab_read.addr[3:0];

    // Access strobes (one fabric cycle per Apple access). Writes latch on
    // data_en (when ab_read.data is valid). Read side effects fire on sss_en
    // -- the same phase the read data is presented -- so the presented
    // (pre-advance) value and the state update sample the same registers on one
    // clock edge, with no dependence on data_en-vs-sss_en ordering.
    wire wr_stb = io_hit && ab_read.data_en && !ab_read.rw;  // CPU write
    wire rd_stb = io_hit && ab_read.serve_en &&  ab_read.rw;  // CPU read

    // Register map (matches the a2fpga SuperSprite decode):
    //   +0  VDP VRAM data        (mode=0)
    //   +1  VDP register/address (mode=1)
    //   +2  speech (not implemented)
    //   +3  APPLE video off      +4 APPLE video on
    //   +5  VDP overlay off      +6 VDP overlay on (mix)
    //   +7  VDP reset
    //   +C..+F  PSG (handled in apple_top, not here)
    localparam [3:0] OFF_VDP_DATA = 4'h0;
    localparam [3:0] OFF_VDP_CTRL = 4'h1;
    localparam [3:0] OFF_SW_APPLE_OFF = 4'h3;
    localparam [3:0] OFF_SW_APPLE_ON  = 4'h4;
    localparam [3:0] OFF_SW_VDP_OFF   = 4'h5;
    localparam [3:0] OFF_SW_VDP_MIX   = 4'h6;
    localparam [3:0] OFF_VDP_RESET    = 4'h7;

    wire vdp_data_hit = (off == OFF_VDP_DATA); // $C0n0
    wire vdp_ctrl_hit = (off == OFF_VDP_CTRL); // $C0n1

    wire vdp_data_wr = wr_stb && vdp_data_hit;
    wire vdp_ctrl_wr = wr_stb && vdp_ctrl_hit;
    wire vdp_data_rd = rd_stb && vdp_data_hit;
    wire vdp_ctrl_rd = rd_stb && vdp_ctrl_hit; // status read

    wire soft_reset = wr_stb && (off == OFF_VDP_RESET);
    wire vdp_reset  = !rstn || !card_enabled || !ab_read.res || soft_reset;

    // ---- VDP registers R0..R7 and soft switches ----
    logic [7:0] regs [0:7];
    logic       apple_video_q;
    logic       vdp_overlay_q;
    genvar gi;
    generate
        for (gi = 0; gi < 8; gi++) begin : g_regpack
            assign ps_regs[gi*8 +: 8] = regs[gi];
        end
    endgenerate
    assign ps_apple_video = apple_video_q;
    assign ps_vdp_overlay = vdp_overlay_q;

    // ---- VRAM (16 KB, dual-port BRAM) ----
    (* ram_style = "block" *) logic [7:0] vram [0:16383];
    logic [13:0] addr_q;        // 14-bit auto-increment address pointer
    logic [7:0]  read_buffer_q; // TMS9918 read-ahead buffer
    logic [7:0]  vram_bus_dout; // registered read at addr_q (bus side)
    logic        addr_ff_q;     // 0 = expect low byte, 1 = expect high byte
    logic [7:0]  addr_temp_q;   // first (low) byte of the two-byte sequence
    logic [1:0]  prefetch_dly_q;// read-ahead load countdown after a read setup

    // True dual-port BRAM. Port A (Apple bus): write on vdp_data_wr with a
    // registered read at addr_q. Port B (PS): registered read at ps_vram_addr.
    // The 1-cycle read latency is hidden by the ~130:1 fabric:bus clock ratio.
    always_ff @(posedge clk) begin
        if (vdp_data_wr) vram[addr_q] <= ab_read.data;
        vram_bus_dout <= vram[addr_q];
    end
    always_ff @(posedge clk) ps_vram_data <= vram[ps_vram_addr];

    // ---- Status register (bit7 = F frame flag; 6:0 from the PS renderer) ----
    logic frame_flag_q;
    assign ps_status      = {frame_flag_q, ps_status_flags};
    assign ps_status_read = vdp_ctrl_rd;
    // INT is asserted while the frame flag is set and R1 bit5 (IE) is 1.
    assign irq_n = !(card_enabled && frame_flag_q && regs[1][5]);

    // ---- AY-3-8910 PSG (SuperSprite sound), $C0nC..$C0nF ----
    // addr[3:2]==11 selects the PSG; addr[1] picks register-latch (1) vs
    // data (0), matching the a2fpga SuperSprite decode.
    wire addr_psg     = (off[3:2] == 2'b11);
    wire psg_data_wr  = wr_stb && addr_psg && !off[1];               // +C/+D
    wire psg_addr_wr  = wr_stb && addr_psg &&  off[1];               // +E/+F
    wire psg_data_rd  = io_hit && ab_read.serve_en && ab_read.rw &&
                        addr_psg && off[1];
    // ~1 MHz PSG clock, once per Apple bus cycle (matches mockingboard).
    wire psg_ce = card_enabled && ab_read.data_en;

    logic [7:0] psg_dout;
    logic [7:0] psg_ch_a, psg_ch_b, psg_ch_c;

    YM2149 ssp_psg (
        .CLK(clk),
        .CE(psg_ce),
        .RESET(vdp_reset),
        .BDIR(psg_addr_wr || psg_data_wr),
        .BC(psg_data_rd || psg_addr_wr),
        .DI(ab_read.data),
        .DO(psg_dout),
        .CHANNEL_A(psg_ch_a),
        .CHANNEL_B(psg_ch_b),
        .CHANNEL_C(psg_ch_c),
        .SEL(1'b0),
        .MODE(1'b0),
        .ACTIVE(),
        .IOA_in(8'h00), .IOA_out(),
        .IOB_in(8'h00), .IOB_out()
    );

    // Mono sum of the three channels (max 765), left-shifted into the signed
    // 16-bit range with headroom for mixing at the top level.
    wire [9:0] psg_sum = {2'b0, psg_ch_a} + {2'b0, psg_ch_b} + {2'b0, psg_ch_c};
    logic signed [15:0] ssp_audio_q;
    assign ssp_audio = ssp_audio_q;

    always_ff @(posedge clk) begin
        if (vdp_reset) begin
            ssp_audio_q <= 16'sd0;
        end else begin
            ssp_audio_q <= card_enabled ? $signed({1'b0, psg_sum, 5'b0}) : 16'sd0;
        end
    end

    // ---- Control / VRAM state machine ----
    always_ff @(posedge clk) begin
        if (vdp_reset) begin
            addr_q        <= 14'd0;
            read_buffer_q <= 8'h00;
            addr_ff_q     <= 1'b0;
            addr_temp_q   <= 8'h00;
            prefetch_dly_q<= 2'd0;
            for (int i = 0; i < 8; i++) regs[i] <= 8'h00;
            apple_video_q <= 1'b1;   // Apple video visible by default
            vdp_overlay_q <= 1'b0;   // overlay off until software enables it
            frame_flag_q  <= 1'b0;
        end else begin
            // Frame flag: set at vblank, cleared by a status read.
            if (vblank_tick)        frame_flag_q <= 1'b1;
            else if (vdp_ctrl_rd)   frame_flag_q <= 1'b0;

            // Read-ahead prefetch: two cycles after a read-address setup the
            // registered VRAM read (vram_bus_dout) has settled to vram[addr_q]
            // for the new address, so latch it and pre-increment.
            if (prefetch_dly_q != 2'd0) begin
                prefetch_dly_q <= prefetch_dly_q - 2'd1;
                if (prefetch_dly_q == 2'd1) begin
                    read_buffer_q <= vram_bus_dout; // = vram[addr_q]
                    addr_q        <= addr_q + 14'd1;
                end
            end

            // ---- VRAM data port ($C0n0) ----
            if (vdp_data_wr) begin
                // write handled in the dedicated always_ff above; advance addr
                addr_q    <= addr_q + 14'd1;
                addr_ff_q <= 1'b0;
            end else if (vdp_data_rd) begin
                // Present read_buffer_q (done combinationally below); refill.
                read_buffer_q <= vram_bus_dout; // = vram[addr_q]
                addr_q        <= addr_q + 14'd1;
                addr_ff_q     <= 1'b0;
            end

            // ---- Control port ($C0n1) ----
            if (vdp_ctrl_wr) begin
                if (!addr_ff_q) begin
                    addr_temp_q <= ab_read.data;   // low byte
                    addr_ff_q   <= 1'b1;
                end else begin
                    addr_ff_q <= 1'b0;
                    unique case (ab_read.data[7:6])
                        2'b00: begin // set VRAM read address, then prefetch
                            addr_q         <= {ab_read.data[5:0], addr_temp_q};
                            prefetch_dly_q <= 2'd2;
                        end
                        2'b01: begin // set VRAM write address
                            addr_q <= {ab_read.data[5:0], addr_temp_q};
                        end
                        2'b10: begin // write register R[data[2:0]] = temp
                            regs[ab_read.data[2:0]] <= addr_temp_q;
                        end
                        default: ; // 2'b11 unused on TMS9918
                    endcase
                end
            end else if (vdp_ctrl_rd) begin
                // Reading status resets the address flip-flop (frame flag
                // cleared above).
                addr_ff_q <= 1'b0;
            end

            // ---- Video soft switches ($C0n3..$C0n6, write-triggered) ----
            if (wr_stb) begin
                unique case (off)
                    OFF_SW_APPLE_OFF: apple_video_q <= 1'b0;
                    OFF_SW_APPLE_ON:  apple_video_q <= 1'b1;
                    OFF_SW_VDP_OFF:   vdp_overlay_q <= 1'b0;
                    OFF_SW_VDP_MIX:   vdp_overlay_q <= 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // ---- Frame counter for the PS (free-running, wraps) ----
    logic [15:0] frame_q;
    always_ff @(posedge clk) begin
        if (!rstn)            frame_q <= 16'd0;
        else if (vblank_tick) frame_q <= frame_q + 16'd1;
    end
    assign ps_frame = frame_q;

    // ---- Apple bus read data presentation ----
    // Present VDP data-port -> read_buffer, control-port -> status.
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write_q <= '0;
        end else begin
            ab_write_q.wr_addr       <= 16'h0000;
            ab_write_q.wr_rw         <= 1'b0;
            ab_write_q.wr_addr_rw_en <= 1'b0;
            ab_write_q.assert_inh    <= 1'b0;
            ab_write_q.assert_res    <= 1'b0;
            ab_write_q.assert_rdy    <= 1'b0;
            ab_write_q.assert_nmi    <= 1'b0;
            ab_write_q.assert_dma    <= 1'b0;
            // IRQ to the bus arbiter (active-high assert form).
            ab_write_q.assert_irq    <= card_enabled && ab_read.res &&
                                        frame_flag_q && regs[1][5];

            if (!card_enabled || ab_read.data_en || ab_read.addr_en) begin
                ab_write_q.wr_data    <= 8'h00;
                ab_write_q.wr_data_en <= 1'b0;
            end

            if (io_hit && ab_read.serve_en && ab_read.rw) begin
                if (vdp_data_hit) begin
                    ab_write_q.wr_data    <= read_buffer_q;
                    ab_write_q.wr_data_en <= 1'b1;
                end else if (vdp_ctrl_hit) begin
                    ab_write_q.wr_data    <= {frame_flag_q, ps_status_flags};
                    ab_write_q.wr_data_en <= 1'b1;
                end else if (addr_psg && off[1]) begin  /* PSG data read $C0nE/F */
                    ab_write_q.wr_data    <= psg_dout;
                    ab_write_q.wr_data_en <= 1'b1;
                end
            end
        end
    end

endmodule
