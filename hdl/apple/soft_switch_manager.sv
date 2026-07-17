module soft_switch_manager (
    input  logic                       clk,
    input  logic                       rstn,
    input  logic                       ramworks_en,
    input  globals::AppleBus_read      ab_read,
    output globals::SoftSwitchState    sss
);

    import globals::*;


    // ---- Soft-switch C0xx address map (addr[7:1]) ----
    localparam logic [6:0] SS80STORE  = 7'h00;
    localparam logic [6:0] AUXREAD    = 7'h01;
    localparam logic [6:0] AUXWRITE   = 7'h02;
    localparam logic [6:0] INTCXROM   = 7'h03;
    localparam logic [6:0] ALTZP      = 7'h04;
    localparam logic [6:0] SLOTC3ROM  = 7'h05;
    localparam logic [6:0] EIGHTYCOL  = 7'h06;
    localparam logic [6:0] ALTCHARSET = 7'h07;
    localparam logic [6:0] TEXT       = 7'h28;
    localparam logic [6:0] MIXED      = 7'h29;
    localparam logic [6:0] PAGE2      = 7'h2a;
    localparam logic [6:0] HIRES      = 7'h2b;
    localparam logic [6:0] DHIRES     = 7'h2f;

    // ---- Registered state ----
    logic        ss_80store;
    logic        ss_auxread;
    logic        ss_auxwrite;
    logic        ss_intcxrom;
    logic        ss_altzp;
    logic        ss_slotc3rom;
    logic        ss_page2;
    logic        ss_hires;
    logic        ss_text;
    logic        ss_mixed;
    logic        ss_altcharset;
    logic        ss_80col;
    logic        ss_dhires;
    logic        ss_lcram_bank2;
    logic        ss_lcram_write;
    logic        ss_lcram_write_last;
    logic        ss_lcram_read;
    logic [2:0]  ss_c8_select;
    logic [7:0]  ss_io_select;
    logic        ss_c8_internal_rom;
    logic [23:0] ss_addr_decode;
    logic        ss_addr_decode_en;
    logic [23:0] ss_addr_decode_late;
    logic        ss_addr_decode_late_en;
    apple_route_kind_e ss_route_kind;
    logic [6:0]  ss_ramworks_bank;

    // ---- Address decoding helpers (combinational) ----
    wire is_c0xx     = (ab_read.addr[15:8]  == 8'hc0);
    wire is_c08x     = (ab_read.addr[15:4]  == 12'hc08);
    wire is_cxxx     = (ab_read.addr[15:12] == 4'hc);
    wire is_zp_stack = (ab_read.addr[15:9]  == 7'h00);          // 0000-01ff
    wire is_high_ram = (ab_read.addr[15:14] == 2'b11);          // c000-ffff (cxxx filtered separately)
    wire is_hires_pg = (ab_read.addr[15:13] == 3'b001);         // 2000-3fff

    // ---- Bank select (combinational decode of current address) ----
    // Selects which 64K physical bank the access targets:
    //   0        : main memory
    //   1..128   : aux/RamWorks banks (8192KB card, including IIe aux)
    // ss_ramworks_bank is 7 bits (0-127); the +1 mapping yields bank_sel
    // 1-128, which requires 8 bits and lands in ss_addr_decode[23:16].
    // 80STORE+PAGE2 overrides AUXREAD/AUXWRITE for display windows
    // ($0400-$07FF text, $2000-$3FFF HGR) and routes them to the currently
    // selected RamWorks bank. The video fetcher intentionally reads fixed
    // bank 1 for aux display fetches, matching the Apple IIe scanner path.
    //
    // M2B0 is intentionally not used for real memory steering here. The
    // current Apple //e fake-SHR path is selected only by C029 and reads the
    // captured AUX $2000-$9FFF shadow; there is no IIgs M2B0 bank source.
    // Translate a 16-bit Apple address using the registered soft switches.
    // decoded_addr carries the selected 64K bank and Apple offset for memory
    // cycles, or a ROM offset for motherboard-ROM reads. route_kind identifies
    // whether memory, motherboard ROM, or bus/card I/O owns the cycle.
    function automatic void translate_apple_addr(
        input  logic [15:0]   addr_in,
        input  logic          rw_in,        // 1=read, 0=write
        output logic [31:0]   decoded_addr,
        output apple_route_kind_e route_kind
    );
        logic        q_is_cxxx;
        logic        q_is_c0xx;
        logic        q_is_zp_stack;
        logic        q_is_high_ram;
        logic        q_is_hires_pg;
        logic        q_is_text_pg1;
        logic        q_is_display_window;
        logic [7:0]  q_aux_bank_full;
        logic [7:0]  q_bank_sel;
        logic [23:0] q_psram_addr;
        logic        q_cxxx_intcxrom_rom;
        logic        q_cxxx_slot3_rom;
        logic        q_extrom_intcxrom;
        logic        q_extrom_internal_rom;
        logic        q_lcrange_rom_read;

        q_is_cxxx      = (addr_in[15:12] == 4'hc);
        q_is_c0xx      = (addr_in[15:8]  == 8'hc0);
        q_is_zp_stack  = (addr_in[15:9]  == 7'h00);    // 0000-01ff
        q_is_high_ram  = (addr_in[15:14] == 2'b11);    // c000-ffff
        q_is_hires_pg  = (addr_in[15:13] == 3'b001);   // 2000-3fff
        q_is_text_pg1  = (addr_in[15:10] == 6'b000001); // 0400-07ff
        q_is_display_window = q_is_text_pg1 || (ss_hires && q_is_hires_pg);
        q_aux_bank_full = {1'b0, ss_ramworks_bank} + 8'd1;

        // ---- Physical 64K bank selection ----
        if (q_is_cxxx) begin
            q_bank_sel = 8'd0;
        end
        else if (q_is_zp_stack) begin
            q_bank_sel = ss_altzp ? q_aux_bank_full : 8'd0;
        end
        else if (q_is_high_ram) begin
            q_bank_sel = ss_altzp ? q_aux_bank_full : 8'd0;
        end
        else begin
            q_bank_sel = (rw_in ? ss_auxread : ss_auxwrite)
                         ? q_aux_bank_full : 8'd0;
            if (ss_80store && q_is_display_window) begin
                q_bank_sel = ss_page2 ? q_aux_bank_full : 8'd0;
            end
        end

        // ---- PSRAM byte address, including the LC bank-1 bit-12 remap.
        //      Bits 23:16 carry bank_sel.
        q_psram_addr      = {8'b0, addr_in};
        if (q_is_high_ram && !ss_lcram_bank2 && addr_in[13:12] == 2'b01) begin
            q_psram_addr[12] = 1'b0;
        end
        q_psram_addr[23:16] = q_bank_sel;

        // ---- Route selection ----
        //
        // ROM cases. Reads only (writes never go to ROM).
        //   $C100-$C7FF read with intcxrom            -> ROM
        //   $C300-$C3FF read with !slotc3rom          -> ROM (slot-3 internal)
        //   $C800-$CFFF read with intcxrom            -> ROM (expansion)
        //   $C800-$CFFF read after internal $C3xx     -> ROM (slot-3 exp)
        //   $D000-$FFFF read with !sw_lcram_read      -> ROM (LC-ROM)
        q_cxxx_intcxrom_rom = q_is_cxxx && !q_is_c0xx && rw_in &&
                              ss_intcxrom &&
                              (addr_in[11:8] >= 4'h1 && addr_in[11:8] <= 4'h7);
        q_cxxx_slot3_rom    = q_is_cxxx && rw_in && !ss_slotc3rom &&
                              (addr_in[11:8] == 4'h3);
        q_extrom_intcxrom   = q_is_cxxx && rw_in && ss_intcxrom &&
                              (addr_in[11:8] >= 4'h8 && addr_in[11:8] <= 4'hF);
        q_extrom_internal_rom = q_is_cxxx && rw_in && !ss_intcxrom &&
                                ss_c8_internal_rom &&
                                (addr_in[11:8] >= 4'h8 && addr_in[11:8] <= 4'hF);
        q_lcrange_rom_read  = q_is_high_ram && !q_is_cxxx && rw_in &&
                              !ss_lcram_read;

        if (q_is_c0xx) begin
            // $C000-$C0FF soft switches are owned by the Apple bus.
            decoded_addr = {16'h0000, addr_in};
            route_kind   = APPLE_ROUTE_BUS;
        end
        else if (q_cxxx_intcxrom_rom || q_cxxx_slot3_rom ||
                 q_extrom_intcxrom   || q_extrom_internal_rom ||
                 q_lcrange_rom_read) begin
            // ROM-bound read. ROM offset = addr - $C000 = addr[13:0]
            // because addr[15:14] == 2'b11 in all ROM-range addresses.
            decoded_addr = {ADDR_REGION_ROM, 10'd0, addr_in[13:0]};
            route_kind   = APPLE_ROUTE_ROM;
        end
        else if (q_is_cxxx) begin
            // $C100-$CFFF non-ROM cases are motherboard or virtual-card I/O.
            decoded_addr = {16'h0000, addr_in};
            route_kind   = APPLE_ROUTE_BUS;
        end
        else if (q_is_high_ram) begin
            // $D000-$FFFF reaches language-card RAM only when the matching
            // read or write latch is enabled. Other accesses remain bus-owned;
            // motherboard ROM reads were classified above.
            if (rw_in && ss_lcram_read) begin
                decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
                route_kind   = APPLE_ROUTE_CACHE;
            end
            else if (!rw_in && ss_lcram_write) begin
                decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
                route_kind   = APPLE_ROUTE_CACHE;
            end
            else begin
                // Disabled language-card writes are ignored by the motherboard.
                decoded_addr = {16'h0000, addr_in};
                route_kind   = APPLE_ROUTE_BUS;
            end
        end
        else begin
            // $0000-$BFFF memory. Bank 0 remains on the motherboard; selected
            // aux/RamWorks banks are served by psram_simple.
            decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
            route_kind   = APPLE_ROUTE_CACHE;
        end
    endfunction

    // Register the translation with the soft-switch state for downstream
    // capture, INH arbitration, and PSRAM service.

    always_ff @(posedge clk) begin
        if (!rstn) begin
            // Hard reset
            ss_80store             <= 1'b0;
            ss_auxread             <= 1'b0;
            ss_auxwrite            <= 1'b0;
            ss_intcxrom            <= 1'b0;
            ss_altzp               <= 1'b0;
            ss_slotc3rom           <= 1'b0;
            ss_page2               <= 1'b0;
            ss_hires               <= 1'b0;
            ss_text                <= 1'b1;  // text mode at reset
            ss_mixed               <= 1'b0;
            ss_altcharset          <= 1'b0;
            ss_80col               <= 1'b0;
            ss_dhires              <= 1'b0;
            ss_lcram_bank2         <= 1'b1;
            ss_lcram_write         <= 1'b1;
            ss_lcram_write_last    <= 1'b0;
            ss_lcram_read          <= 1'b0;
            ss_c8_select           <= 3'h0;
            ss_io_select           <= 8'h00;
            ss_c8_internal_rom      <= 1'b0;
            ss_addr_decode         <= '0;
            ss_addr_decode_en      <= 1'b0;
            ss_addr_decode_late    <= '0;
            ss_addr_decode_late_en <= 1'b0;
            ss_route_kind          <= APPLE_ROUTE_INVALID;
            ss_ramworks_bank       <= 7'b0;
        end
        else begin
            // -------- RamWorks bank select (C071/C073) --------
            // Captured at data_en time when the data bus carries the value.
            // Values > 127 are ignored (rather than masked).
            /* RamWorks bank select ($C071/$C073, data bit7=0). Gated:
             * these addresses sit in the //e's paddle-trigger region,
             * so with the feature off a stray write must not re-bank
             * auxiliary memory. With it on, the full 127 extra banks
             * (8 MB with base aux) are exposed; software sizes the
             * card by probing. */
            if (ramworks_en && ab_read.data_en && !ab_read.rw && is_c0xx
                && (ab_read.addr[7:0] == 8'h71 || ab_read.addr[7:0] == 8'h73)
                && ab_read.data[7] == 1'b0)
                ss_ramworks_bank <= ab_read.data[6:0];
            /* bank_sel 0x80 supplies the final RamWorks bank through PSRAM
             * address bit 23. Keep that bit in the translated address. */

            // -------- INH-path early decode (addr_en) --------
            // The PSRAM/INH serving arm must decide before the
            // TAP_INH_DEADLINE mid-PHI1, so it translates the early
            // snapshot. Only a 6502-timed master is guaranteed valid
            // here; that is inherent to INH serving and acceptable --
            // a DMA-mastered machine (TransWarp) shadows the memory
            // this arm would serve.
            if (ab_read.addr_en && ab_read.cycle_valid) begin : addr_decode_translator
                logic [31:0]        rb_decoded_addr;
                apple_route_kind_e  rb_route_kind;
                translate_apple_addr(ab_read.addr_early, ab_read.rw_early,
                                     rb_decoded_addr, rb_route_kind);
                ss_addr_decode_en  <= (rb_route_kind == APPLE_ROUTE_CACHE);
                ss_addr_decode     <= rb_decoded_addr[23:0];
                ss_route_kind      <= rb_route_kind;
            end

            // -------- Soft switches, claims, observation decode --------
            // Keyed on serve_en: ab_read.addr/rw are the PHI0-high sample,
            // valid for any master the motherboard itself can accept.
            // Applied exactly once per cycle, so sequence-sensitive state
            // (the LC C08x write-enable) is safe here.
            if (ab_read.serve_en && ab_read.cycle_valid) begin
                // -------- C0xx direct soft switches --------
                if (is_c0xx) begin
                    unique case (ab_read.addr[7:1])
                        SS80STORE: if (!ab_read.rw) ss_80store   <= ab_read.addr[0];
                        AUXREAD:   if (!ab_read.rw) ss_auxread   <= ab_read.addr[0];
                        AUXWRITE:  if (!ab_read.rw) ss_auxwrite  <= ab_read.addr[0];
                        INTCXROM:  if (!ab_read.rw) ss_intcxrom  <= ab_read.addr[0];
                        ALTZP:     if (!ab_read.rw) ss_altzp     <= ab_read.addr[0];
                        SLOTC3ROM:  if (!ab_read.rw) ss_slotc3rom  <= ab_read.addr[0];
                        EIGHTYCOL:  if (!ab_read.rw) ss_80col      <= ab_read.addr[0];
                        ALTCHARSET: if (!ab_read.rw) ss_altcharset <= ab_read.addr[0];
                        TEXT:                        ss_text       <= ab_read.addr[0];
                        MIXED:                       ss_mixed      <= ab_read.addr[0];
                        PAGE2:                       ss_page2      <= ab_read.addr[0];
                        HIRES:                       ss_hires      <= ab_read.addr[0];
                        DHIRES:                      ss_dhires     <= ~ab_read.addr[0];
                        default: ;
                    endcase
                end

                // -------- Language card soft switches (C08x) --------
                if (is_c08x) begin
                    ss_lcram_write_last    <= ab_read.addr[0] && ab_read.rw;
                    ss_lcram_bank2         <= ~ab_read.addr[3];
                    ss_lcram_read          <= (ab_read.addr[1] == ab_read.addr[0]);
                    if (ab_read.addr[0]) begin
                        if (ss_lcram_write_last && ab_read.rw)
                            ss_lcram_write <= 1'b1;
                    end
                    else begin
                        ss_lcram_write <= 1'b0;
                    end
                end

                // -------- Slot select / C8 expansion ROM --------
                if (is_cxxx) begin
                    // C1xx-C7xx: slot select. INTC8ROM is sticky: a slot Cn
                    // access must not clear it. Once internal C3 firmware
                    // claims $C800-$CFFF, the motherboard owns that range
                    // until a $CFFF access or reset and holds I/O STROBE
                    // inactive so slot cards cannot drive C8 space.
                    if (ab_read.addr[11] == 1'b0 && ab_read.addr[10:8] != 3'h0) begin
                        // Slot claim only with INTCXROM off. A C3xx access
                        // with SLOTC3ROM off latches the slot-3
                        // internal ROM in both INTCXROM states. This keeps C8
                        // internal if INTCXROM is subsequently cleared.
                        if (!ss_intcxrom) begin
                            ss_c8_select   <= ab_read.addr[10:8];
                            ss_io_select[ab_read.addr[10:8]] <= 1'b1;
                        end
                        if (ab_read.addr[10:8] == 3'h3 && !ss_slotc3rom) begin
                            ss_c8_select   <= 3'h0;
                            ss_io_select[3] <= 1'b0;
                            ss_c8_internal_rom <= 1'b1;
                        end
                    end
                    // CFFF releases C8xx expansion-ROM ownership
                    if (ab_read.addr == 16'hcfff) begin
                        ss_c8_select <= 3'h0;
                        ss_io_select <= 8'h00;
                        ss_c8_internal_rom <= 1'b0;
                    end
                end

                // -------- Observation address decode --------
                // translate_apple_addr() applies bank selection,
                // language-card remapping, and route classification from
                // the current state. This is the decode the capture path
                // consumes at data_en; the INH/serving decode above is its
                // early-snapshot counterpart. Uses the pre-update switch
                // state (same-edge semantics), matching real-hardware
                // behavior where an access takes effect after its cycle.
                begin : obs_decode_translator
                    logic [31:0]        rb_decoded_addr_obs;
                    apple_route_kind_e  rb_route_kind_obs;
                    translate_apple_addr(ab_read.addr, ab_read.rw,
                                         rb_decoded_addr_obs, rb_route_kind_obs);
                    ss_addr_decode_late_en <= (rb_route_kind_obs == APPLE_ROUTE_CACHE);
                    ss_addr_decode_late    <= rb_decoded_addr_obs[23:0];
                end
            end

            // Soft reset (apple-side RES) -- overrides updates this cycle
            if (!ab_read.res) begin
                ss_80store             <= 1'b0;
                ss_auxread             <= 1'b0;
                ss_auxwrite            <= 1'b0;
                ss_intcxrom            <= 1'b0;
                ss_altzp               <= 1'b0;
                ss_slotc3rom           <= 1'b0;
                ss_page2               <= 1'b0;
                ss_hires               <= 1'b0;
                ss_text                <= 1'b1;
                ss_mixed               <= 1'b0;
                ss_altcharset          <= 1'b0;
                ss_80col               <= 1'b0;
                ss_dhires              <= 1'b0;
                ss_lcram_bank2         <= 1'b1;
                ss_lcram_write         <= 1'b1;
                ss_lcram_write_last    <= 1'b0;
                ss_lcram_read          <= 1'b0;
                ss_c8_select           <= 3'h0;
                ss_io_select           <= 8'h00;
                ss_c8_internal_rom      <= 1'b0;
                    ss_addr_decode         <= '0;
                ss_addr_decode_en      <= 1'b0;
                ss_addr_decode_late    <= '0;
                ss_addr_decode_late_en <= 1'b0;
                ss_route_kind          <= APPLE_ROUTE_INVALID;
                // Ctrl-Reset selects base auxiliary bank 0 because Apple
                // startup software assumes that bank while rebuilding its
                // memory state.
                ss_ramworks_bank       <= 7'b0;
            end
        end
    end

    // ---- Outputs (registered state passthrough) ----
    assign sss.addr_decode     = ss_addr_decode;
    assign sss.addr_decode_en  = ss_addr_decode_en;
    assign sss.addr_decode_late    = ss_addr_decode_late;
    assign sss.addr_decode_late_en = ss_addr_decode_late_en;
    assign sss.route_kind      = ss_route_kind;
    /* INTC8ROM exclusion: while the internal C8 ROM owns the window,
     * no slot card may claim it (the //e inhibits I/O STROBE'). */
    assign sss.c8_select      = ss_c8_internal_rom ? 3'h0 : ss_c8_select;
    /* Per-cycle $Cnxx slot-access qualifier, combinational from the
     * authoritative address sample so serve_en-keyed card decode sees
     * it with zero staleness. Uses the registered INTCXROM/SLOTC3ROM
     * state as of this cycle's start (same semantics the registered
     * version had). Only meaningful at/after serve_en; during PHI1
     * ab_read.addr still holds the previous cycle's sample.  */
    assign sss.slot_access =
        (ab_read.addr[15:12] == 4'hc) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] != 3'h0) &&
        !ss_intcxrom &&
        !((ab_read.addr[10:8] == 3'h3) && !ss_slotc3rom);
    assign sss.sw_80store     = ss_80store;
    assign sss.sw_intcxrom    = ss_intcxrom;
    assign sss.sw_slotc3rom   = ss_slotc3rom;
    assign sss.c8_internal_rom = ss_c8_internal_rom;
    /* Per-slot FFs, gated like c8_select: no card drives while the
     * internal ROM owns the window. */
    assign sss.io_select = ss_c8_internal_rom ? 8'h00 : ss_io_select;
    assign sss.sw_ramrd       = ss_auxread;
    assign sss.sw_ramwrt      = ss_auxwrite;
    assign sss.sw_altzp       = ss_altzp;
    assign sss.sw_text        = ss_text;
    assign sss.sw_mixed       = ss_mixed;
    assign sss.sw_page2       = ss_page2;
    assign sss.sw_hires       = ss_hires;
    assign sss.sw_altcharset  = ss_altcharset;
    assign sss.sw_80col       = ss_80col;
    assign sss.sw_dhires      = ss_dhires;
    assign sss.sw_lcram_bank2 = ss_lcram_bank2;
    assign sss.sw_lcram_read  = ss_lcram_read;
    assign sss.sw_lcram_write = ss_lcram_write;
    assign sss.sw_ramworks_bank = ss_ramworks_bank;

endmodule
