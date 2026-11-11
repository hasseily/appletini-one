module soft_switch_manager (
    input  logic                       clk,
    input  logic                       rstn,
    input  logic                       ramworks_en,
    input  logic                       iigs_profile,
    input  logic                       iigs_bootstrap_slot7,
    input  logic [7:0]                 iigs_boot_mask,
    input  logic                       iigs_boot_mask_valid,
    input  logic                       iigs_native_video,
    input  logic                       iigs_bank_latch,
    input  globals::AppleBus_read      ab_read,
    output globals::SoftSwitchState    sss,
    output logic [7:0]                 iigs_live_mask,
    output logic                       iigs_live_mask_valid,
    output logic [7:0]                 iigs_io_mask,
    output logic [7:0]                 iigs_rom_mask
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
    logic        iigs_report_seen;
    logic        iigs_profile_q;
    logic [2:0]  iigs_c8_slot;
    logic        iigs_c8_internal;

    // C02D has no slot-3 bit. Slot 3 follows SLOTC3ROM for Cn;
    // slots 3 and 4 have permanently enabled device I/O.
    // Keep the default bootstrap grant through the edge that imports the
    // report. Once consumed, the old report cannot reopen it after isolation.
    wire iigs_legacy_bootstrap = iigs_bootstrap_slot7 && !iigs_report_seen;
    wire [7:0] iigs_selected_mask = iigs_live_mask_valid ?
        iigs_live_mask : (iigs_legacy_bootstrap ? 8'h80 : 8'h00);
    // Internal next-state logic uses the unregistered masks.
    wire [7:0] iigs_io_mask_now = iigs_selected_mask | 8'h18;
    wire [7:0] iigs_rom_mask_now = ss_intcxrom ? 8'h00 :
        (iigs_selected_mask | (ss_slotc3rom ? 8'h08 : 8'h00));

    /* Registered copies for card decode. Every card decodes C8 ownership
     * and slot access on its bus strobes, so these must not add mask
     * logic to those paths. The copies follow their register inputs one
     * clock later. The inputs change only at bus strobes, PS writes, or
     * policy changes, and bus strobes are at least 30 clocks apart, so a
     * strobe always sees settled values. */
    logic [7:0] iigs_io_mask_q    = 8'h18;
    logic [7:0] iigs_rom_mask_q   = 8'h00;
    logic [2:0] iigs_c8_owner_q   = 3'd0;
    logic [7:0] iigs_io_select_q  = 8'h00;
    logic [7:0] cn_rom_visible_q  = 8'hF6;  // Cn ROM visibility, per slot
    assign iigs_io_mask = iigs_io_mask_q;
    assign iigs_rom_mask = iigs_rom_mask_q;
    wire iigs_valid_cycle = ab_read.cycle_valid && !ab_read.m2sel;
    wire iigs_state_write = iigs_profile && iigs_valid_cycle &&
        ab_read.data_en && !ab_read.rw && ab_read.addr == 16'hC068;
    wire iigs_slot_write = iigs_profile && iigs_valid_cycle &&
        ab_read.data_en && !ab_read.rw && ab_read.addr == 16'hC02D;
    wire iigs_cn_cycle = ab_read.addr[15:11] == 5'h18 &&
                         ab_read.addr[10:8] != 3'd0;
    wire [2:0] iigs_cn_slot = ab_read.addr[10:8];

    always_ff @(posedge clk) begin
        iigs_profile_q <= rstn && iigs_profile;
        if (!rstn) begin
            iigs_report_seen <= 1'b0;
            iigs_live_mask <= 8'h00;
            iigs_live_mask_valid <= 1'b0;
        end else if (!iigs_profile) begin
            // Losing observation invalidates the live map. An old report
            // cannot seed it again after a private ONE//e session.
            iigs_report_seen <= iigs_report_seen || iigs_boot_mask_valid;
            iigs_live_mask <= 8'h00;
            iigs_live_mask_valid <= 1'b0;
        end else begin
            if (iigs_boot_mask_valid && !iigs_report_seen) begin
                iigs_report_seen <= 1'b1;
                if (!iigs_live_mask_valid) begin
                    iigs_live_mask <= iigs_boot_mask & 8'hF6;
                    iigs_live_mask_valid <= 1'b1;
                end
            end
            if (ab_read.res && iigs_slot_write) begin
                iigs_live_mask <= ab_read.data & 8'hF6;
                iigs_live_mask_valid <= 1'b1;
            end
        end

        // A remapped slot must touch Cn again to claim C8 after re-enable.
        // Also revoke a claim on a global internal-ROM override. An
        // internal CFFF can occur while the physical bus is unselected.
        // The GS has one C8 owner. An observed internal C3 claim is sticky
        // until a selected CFFF access or reset.
        if (!rstn || !iigs_profile || !ab_read.res) begin
            iigs_c8_slot <= 3'd0;
            iigs_c8_internal <= 1'b0;
        end else if (!iigs_profile_q && iigs_legacy_bootstrap) begin
            // The default profile can first discover the GS inside the C7
            // boot session. Retain that session's already observed claim.
            iigs_c8_slot <= ss_io_select[7] ? 3'd7 : 3'd0;
            iigs_c8_internal <= ss_c8_internal_rom;
        end else begin
            if (!iigs_rom_mask_now[iigs_c8_slot] ||
                (iigs_slot_write && iigs_c8_slot != 3'd3 &&
                 !ab_read.data[iigs_c8_slot]) ||
                (iigs_state_write && ab_read.data[0]))
                iigs_c8_slot <= 3'd0;
            if (ab_read.serve_en && iigs_valid_cycle) begin
                if (iigs_cn_cycle) begin
                    iigs_c8_slot <= iigs_rom_mask_now[iigs_cn_slot] ?
                        iigs_cn_slot : 3'd0;
                    if (iigs_cn_slot == 3'd3 && !ss_slotc3rom)
                        iigs_c8_internal <= 1'b1;
                end
                if (ab_read.addr == 16'hCFFF) begin
                    iigs_c8_slot <= 3'd0;
                    iigs_c8_internal <= 1'b0;
                end
            end
        end
    end

    // Card-facing copies (see the note at iigs_io_mask_q). The C8 owner
    // follows iigs_c8_slot one clock later; the legacy path is unchanged.
    wire [2:0] iigs_c8_owner_now =
        !iigs_c8_internal && iigs_rom_mask_now[iigs_c8_slot] ?
        iigs_c8_slot : 3'd0;
    always_ff @(posedge clk) begin
        iigs_io_mask_q   <= iigs_io_mask_now;
        iigs_rom_mask_q  <= iigs_rom_mask_now;
        iigs_c8_owner_q  <= iigs_c8_owner_now;
        iigs_io_select_q <= (iigs_c8_owner_now == 3'd0) ? 8'h00 :
                            (8'h01 << iigs_c8_owner_now);
        // Slot 0 has no Cn window. Legacy: INTCXROM hides every slot and
        // SLOTC3ROM off hides slot 3. Aligned with iigs_profile_q.
        cn_rom_visible_q <= 8'hFE & (iigs_profile ? iigs_rom_mask_now :
            (ss_intcxrom ? 8'h00 : (ss_slotc3rom ? 8'hFF : 8'hF7)));
    end

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
    // M2B0 is only used by the native GS observation decode below.
    // It cannot grant host memory ownership or change the early INH decode.
    //
    // Address translation lives in globals::translate_apple_addr (shared
    // with the virtual TransWarp's private switch copy). This module feeds
    // it the registered switch state below.
    TranslateState xlate_st;
    always_comb begin
        xlate_st.sw_80store       = ss_80store;
        xlate_st.sw_auxread       = ss_auxread;
        xlate_st.sw_auxwrite      = ss_auxwrite;
        xlate_st.sw_altzp         = ss_altzp;
        xlate_st.sw_page2         = ss_page2;
        xlate_st.sw_hires         = ss_hires;
        xlate_st.sw_intcxrom      = ss_intcxrom;
        xlate_st.sw_slotc3rom     = ss_slotc3rom;
        xlate_st.c8_internal_rom  = ss_c8_internal_rom;
        xlate_st.sw_lcram_bank2   = ss_lcram_bank2;
        xlate_st.sw_lcram_read    = ss_lcram_read;
        xlate_st.sw_lcram_write   = ss_lcram_write;
        xlate_st.sw_ramworks_bank = ss_ramworks_bank;
    end

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
                translate_apple_addr(xlate_st,
                                     ab_read.addr_early, ab_read.rw_early,
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
                    TranslateState obs_state;
                    obs_state = xlate_st;
                    if (iigs_native_video)
                        obs_state.sw_ramworks_bank = 7'd0;
                    translate_apple_addr(obs_state, ab_read.addr, ab_read.rw,
                                         rb_decoded_addr_obs, rb_route_kind_obs);
                    // Apple IIgs Hardware Reference describes NEWVIDEO.0
                    // set as enabling the bank latch. M2B0 clear still
                    // needs RAMRD/RAMWRT/ALTZP and 80STORE/PAGE2 decoding.
                    if (iigs_native_video && !ab_read.m2sel &&
                        iigs_bank_latch && ab_read.m2b0)
                        rb_decoded_addr_obs[23:16] = 8'd1;
                    ss_addr_decode_late_en <= (rb_route_kind_obs == APPLE_ROUTE_CACHE);
                    ss_addr_decode_late    <= rb_decoded_addr_obs[23:0];
                end
            end

            // The GS State register aliases these individual switches.
            if (iigs_state_write) begin
                ss_altzp       <= ab_read.data[7];
                ss_page2       <= ab_read.data[6];
                ss_auxread     <= ab_read.data[5];
                ss_auxwrite    <= ab_read.data[4];
                ss_lcram_read  <= !ab_read.data[3];
                ss_lcram_bank2 <= ab_read.data[2];
                ss_intcxrom    <= ab_read.data[0];
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
    // The GS profile selects registered copies. iigs_profile_q follows
    // the policy one clock later, like the copies themselves.
    assign sss.c8_select = iigs_profile_q ? iigs_c8_owner_q :
        (ss_c8_internal_rom ? 3'h0 : ss_c8_select);
    /* Per-cycle $Cnxx slot-access qualifier, combinational from the
     * authoritative address sample so serve_en-keyed card decode sees
     * it with zero staleness. The per-slot ROM visibility is registered
     * from INTCXROM/SLOTC3ROM (or the GS ROM mask). Those switches change
     * only at bus strobes, so a serve_en always sees the state as of this
     * cycle's start. Only meaningful at/after serve_en; during PHI1
     * ab_read.addr still holds the previous cycle's sample.  */
    assign sss.slot_access =
        (ab_read.addr[15:11] == 5'h18) &&
        cn_rom_visible_q[ab_read.addr[10:8]] &&
        (!iigs_profile_q || iigs_valid_cycle);
    assign sss.sw_80store     = ss_80store;
    assign sss.sw_intcxrom    = ss_intcxrom;
    assign sss.sw_slotc3rom   = ss_slotc3rom;
    assign sss.c8_internal_rom = iigs_profile_q ? iigs_c8_internal :
        ss_c8_internal_rom;
    /* Per-slot FFs, gated like c8_select: no card drives while the
     * internal ROM owns the window. */
    assign sss.io_select = iigs_profile_q ? iigs_io_select_q :
        (ss_c8_internal_rom ? 8'h00 : ss_io_select);
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
