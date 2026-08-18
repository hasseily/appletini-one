`timescale 1ns / 1ps

module tb_onee_motherboard_io;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic enabled = 1'b0;
    globals::AppleBus_read ab_read = '0;
    globals::AppleBus_read softswitch_ab_read;
    globals::AppleBus_write ab_write;
    globals::SoftSwitchState sss;

    logic [7:0] floating_bus_data = 8'h35;
    logic video_vblank = 1'b0;
    logic keyboard_event_valid = 1'b0;
    wire keyboard_event_ready;
    logic [6:0] keyboard_event_code = 7'h00;
    logic keyboard_any_down = 1'b0;
    logic [2:0] keyboard_modifiers_in = 3'b000;
    wire [2:0] keyboard_modifiers_state;
    wire [7:0] keyboard_latch;
    wire keyboard_strobe;
    logic [2:0] pushbuttons = 3'b000;
    logic cassette_in = 1'b0;
    logic [31:0] paddle_values = 32'h03020100;
    wire cassette_out;
    wire speaker;
    wire utility_strobe_pulse;
    wire [3:0] annunciators;
    wire ioudis;
    wire [3:0] paddle_active;
    wire paddle_trigger_pulse;
    logic last_utility_strobe;
    logic last_keyboard_ready;

    onee_motherboard_io #(
        .PADDLE_BASE_CYCLES(1),
        .PADDLE_SCALE_CYCLES(2)
    ) dut (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .ab_read(ab_read),
        .sss(sss),
        .softswitch_ab_read(softswitch_ab_read),
        .ab_write(ab_write),
        .floating_bus_data(floating_bus_data),
        .video_vblank(video_vblank),
        .keyboard_event_valid(keyboard_event_valid),
        .keyboard_event_ready(keyboard_event_ready),
        .keyboard_event_code(keyboard_event_code),
        .keyboard_any_down(keyboard_any_down),
        .keyboard_modifiers_in(keyboard_modifiers_in),
        .keyboard_modifiers_state(keyboard_modifiers_state),
        .keyboard_latch(keyboard_latch),
        .keyboard_strobe(keyboard_strobe),
        .pushbuttons(pushbuttons),
        .cassette_in(cassette_in),
        .paddle_values(paddle_values),
        .cassette_out(cassette_out),
        .speaker(speaker),
        .utility_strobe_pulse(utility_strobe_pulse),
        .annunciators(annunciators),
        .ioudis(ioudis),
        .paddle_active(paddle_active),
        .paddle_trigger_pulse(paddle_trigger_pulse)
    );

    soft_switch_manager manager (
        .clk(clk),
        .rstn(resetn),
        .ramworks_en(1'b0),
        .ab_read(softswitch_ab_read),
        .sss(sss)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic access(
        input  logic [15:0] addr,
        input  logic        rw,
        input  logic [7:0]  wdata,
        output logic        claimed,
        output logic [7:0]  rdata,
        output logic        triggered
    );
        ab_read.addr     = addr;
        ab_read.addr_early = addr;
        ab_read.rw       = rw;
        ab_read.rw_early = rw;
        ab_read.data     = wdata;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        #1;
        claimed   = ab_write.wr_data_en;
        rdata     = ab_write.wr_data;
        triggered = paddle_trigger_pulse;
        last_utility_strobe = utility_strobe_pulse;
        last_keyboard_ready = keyboard_event_ready;

        ab_read.serve_en = 1'b0;
        ab_read.data_en  = 1'b1;
        @(posedge clk);
        #1;
        ab_read.data_en = 1'b0;
        #1;
    endtask

    task automatic native_tick;
        ab_read.addr    = 16'hFFFF;
        ab_read.rw      = 1'b1;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        ab_read.data_en = 1'b0;
    endtask

    task automatic push_key(input logic [6:0] code);
        check(keyboard_event_ready, "keyboard event input was not ready");
        keyboard_event_code  = code;
        keyboard_event_valid = 1'b1;
        @(posedge clk);
        #1;
        keyboard_event_valid = 1'b0;
    endtask

    logic claimed;
    logic triggered;
    logic [7:0] rdata;
    integer alias_index;

    initial begin
        ab_read.cycle_valid = 1'b1;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        repeat (4) @(posedge clk);
        resetn = 1'b1;
        repeat (2) @(posedge clk);
        #1;

        // Disabled ONE//e is transparent to the shared soft-switch manager,
        // but it cannot accept input, claim data, or cause motherboard I/O.
        check(!keyboard_event_ready && ab_write == '0,
              "disabled ONE//e exposed keyboard or Apple-bus output");
        access(16'hC030, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && !speaker && !triggered && ab_write == '0,
              "disabled ONE//e caused speaker or Apple-bus activity");
        access(16'hC07E, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!ioudis && paddle_active == 4'h0 && !triggered,
              "disabled ONE//e changed IOUDIS or paddle state");
        access(16'hC050, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!sss.sw_text,
              "disabled ONE//e did not pass C050 to the soft-switch manager");
        check(softswitch_ab_read === ab_read,
              "disabled ONE//e changed the soft-switch bus");
        access(16'hC051, 1'b1, 8'h00, claimed, rdata, triggered);
        check(sss.sw_text, "disabled soft-switch restore did not pass through");

        enabled = 1'b1;
        @(posedge clk);
        #1;

        check(!keyboard_strobe && !cassette_out && !speaker,
              "motherboard I/O reset state");
        check(annunciators == 4'h0 && !ioudis,
              "annunciator/IOUDIS reset state");
        check(!ab_write.wr_data_en, "reset left motherboard driving data");

        // Disable must also kill a response which was already registered.
        push_key(7'h40);
        ab_read.addr = 16'hC000;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        #1;
        check(ab_write.wr_data_en, "setup read was not claimed");
        enabled = 1'b0;
        #1;
        check((ab_write == '0) && (softswitch_ab_read === ab_read),
              "disable did not kill a pending response at once");
        ab_read.serve_en = 1'b0;
        @(posedge clk);
        #1;
        check(!keyboard_strobe && !speaker && annunciators == 4'h0,
              "disabled clock did not clear ONE//e-owned state");
        enabled = 1'b1;
        @(posedge clk);
        #1;

        // Keyboard latch, C000 strobe, C010 any-key-down, and modifiers.
        keyboard_modifiers_in = 3'b101;
        keyboard_any_down = 1'b1;
        push_key(7'h41);
        check(keyboard_latch == 8'hC1 &&
              keyboard_modifiers_state == 3'b101,
              "keyboard event did not latch code/strobe/modifiers");
        access(16'hC000, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'hC1,
              "$C000 did not return keyboard data/strobe");
        access(16'hC010, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'hC1,
              "$C010 did not return any-key-down plus last key code");
        check(last_keyboard_ready && !keyboard_strobe &&
              keyboard_latch == 8'h41,
              "$C010 did not clear the strobe or ready the event input");

        keyboard_any_down = 1'b0;
        access(16'hC010, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'h41,
              "$C010 any-key-down did not follow the live key level");

        // Existing SoftSwitchState owns all MMU/video status. Reads at
        // C011-C01F do not clear the keyboard strobe. Writes anywhere in
        // C010-C01F do clear it.
        access(16'hC001, 1'b0, 8'h00, claimed, rdata, triggered);
        check(sss.sw_80store, "$C001 did not reach soft_switch_manager");
        push_key(7'h42);
        access(16'hC018, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'hB5 && keyboard_strobe &&
              !last_keyboard_ready,
              "$C018 status/floating bits or non-destructive read was wrong");
        access(16'hC018, 1'b0, 8'h00, claimed, rdata, triggered);
        check(last_keyboard_ready && !keyboard_strobe,
              "$C018 write did not clear the strobe or ready the event input");
        push_key(7'h43);
        access(16'hC01A, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5 && keyboard_strobe,
              "$C01A did not report TEXT without clearing the keyboard");
        access(16'hC01F, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!keyboard_strobe, "$C01F write did not clear keyboard strobe");
        video_vblank = 1'b1;
        access(16'hC019, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'h35, "$C019 RDVBLBAR polarity was wrong");
        video_vblank = 1'b0;

        // Legacy toggles leave reads floating. The C04X motherboard decoder
        // mirrors the utility strobe across C040-C04F.
        access(16'hC020, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && cassette_out, "$C020 cassette toggle/fallback");
        access(16'hC02F, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!cassette_out, "$C02F cassette alias did not toggle");
        access(16'hC030, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && speaker, "$C030 speaker toggle/fallback");
        access(16'hC03F, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!speaker, "$C03F speaker alias did not toggle");
        access(16'hC040, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && last_utility_strobe,
              "$C040 utility strobe/fallback");
        access(16'hC041, 1'b1, 8'h00, claimed, rdata, triggered);
        check(last_utility_strobe, "$C041 utility-strobe mirror missing");
        access(16'hC04F, 1'b0, 8'h00, claimed, rdata, triggered);
        check(last_utility_strobe, "$C04F utility-strobe mirror missing");
        access(16'hC050, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!last_utility_strobe, "$C050 falsely pulsed utility strobe");

        // C050-C057 are not duplicated here; the forwarded bus feeds the
        // existing manager. Their reads remain floating.
        access(16'hC050, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && !sss.sw_text, "$C050 TEXT off did not reach manager");
        access(16'hC051, 1'b0, 8'h00, claimed, rdata, triggered);
        check(sss.sw_text, "$C051 TEXT on did not reach manager");
        access(16'hC053, 1'b1, 8'h00, claimed, rdata, triggered);
        check(sss.sw_mixed, "$C053 MIXED on did not reach manager");
        access(16'hC055, 1'b1, 8'h00, claimed, rdata, triggered);
        check(sss.sw_page2, "$C055 PAGE2 on did not reach manager");
        access(16'hC057, 1'b1, 8'h00, claimed, rdata, triggered);
        check(sss.sw_hires, "$C057 HIRES on did not reach manager");

        // A //e drives AN0-AN3 on every C058-C05F access. Unlike the //c,
        // C05E/F also reach DHIRES at the same time and no IOUDIS write can
        // gate either effect. This is the normal software path into DHGR.
        access(16'hC059, 1'b1, 8'h00, claimed, rdata, triggered);
        access(16'hC05B, 1'b0, 8'h00, claimed, rdata, triggered);
        access(16'hC05D, 1'b1, 8'h00, claimed, rdata, triggered);
        access(16'hC05F, 1'b1, 8'h00, claimed, rdata, triggered);
        check(annunciators == 4'hF && !sss.sw_dhires,
              "//e C05F did not set AN3 and clear DHIRES");
        access(16'hC05E, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!annunciators[3] && sss.sw_dhires,
              "normal //e C05E did not clear AN3 and set DHIRES");

        // C07E and C07F remain floating paddle triggers on the //e. Neither
        // write creates the //c-only IOUDIS gate.
        access(16'hC07E, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && !ioudis && triggered,
              "//e $C07E was not a floating paddle trigger");
        access(16'hC07E, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!ioudis && triggered,
              "//e $C07E write created an IOUDIS gate");
        access(16'hC058, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!annunciators[0], "//e $C058 did not clear AN0");
        access(16'hC07F, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!claimed && triggered,
              "//e $C07F was not a floating paddle trigger");
        access(16'hC05F, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!sss.sw_dhires && annunciators[3],
              "//e C05F did not set AN3 and clear DHIRES");
        access(16'hC07F, 1'b0, 8'h00, claimed, rdata, triggered);
        check(!ioudis, "//e $C07F write created an IOUDIS gate");
        access(16'hC05E, 1'b1, 8'h00, claimed, rdata, triggered);
        check(!annunciators[3] && sss.sw_dhires,
              "//e C05E did not still update AN3 and DHIRES");

        // Cassette, Apple keys, PB2, and paddle status drive only bit 7.
        // C068-C06F mirror C060-C067 because the selector ignores A3.
        cassette_in = 1'b1;
        pushbuttons = 3'b101;
        access(16'hC060, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'hB5, "$C060 cassette input");
        access(16'hC061, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5, "$C061 Open Apple");
        access(16'hC062, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'h35, "$C062 Closed Apple");
        access(16'hC063, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5, "$C063 PB2");
        access(16'hC068, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'hB5, "$C068 cassette mirror");
        access(16'hC069, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5, "$C069 Open Apple mirror");
        access(16'hC06A, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'h35, "$C06A Closed Apple mirror");
        access(16'hC06B, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5, "$C06B PB2 mirror");

        paddle_values = 32'hFFFFFFFF;
        access(16'hC070, 1'b1, 8'h00, claimed, rdata, triggered);
        for (alias_index = 4; alias_index < 8; alias_index = alias_index + 1) begin
            access(16'hC068 + alias_index, 1'b1, 8'h00,
                   claimed, rdata, triggered);
            check(claimed && rdata == 8'hB5,
                  "$C06C-$C06F paddle mirror was wrong");
        end
        paddle_values = 32'h03020100;

        // Every C070-C07F read or write is a paddle trigger alias. Test all
        // 16 in both directions, then check native-cycle expiry with reloads
        // of 1, 3, 5, and 7 cycles.
        for (alias_index = 0; alias_index < 16; alias_index = alias_index + 1) begin
            access(16'hC070 + alias_index, 1'b1, 8'h00,
                   claimed, rdata, triggered);
            check(triggered, "$C070-$C07F read trigger alias missing");
            access(16'hC070 + alias_index, 1'b0, 8'h00,
                   claimed, rdata, triggered);
            check(triggered, "$C070-$C07F write trigger alias missing");
        end
        check(paddle_active == 4'b1110,
              "paddle snapshot/native decrement after trigger");
        access(16'hC064, 1'b1, 8'h00, claimed, rdata, triggered);
        check(claimed && rdata == 8'h35, "expired PDL0 status");
        access(16'hC065, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5, "active PDL1 status");
        check(paddle_active == 4'b1100, "PDL1 native expiry");
        native_tick();
        native_tick();
        check(paddle_active == 4'b1000, "PDL2 native expiry");
        access(16'hC066, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'h35, "expired PDL2 status");
        access(16'hC067, 1'b1, 8'h00, claimed, rdata, triggered);
        check(rdata == 8'hB5 && paddle_active == 4'b0000,
              "PDL3 status/expiry");

        // Local Apple reset clears motherboard-owned latches without adding
        // a second copy of MMU/video state.
        ab_read.res = 1'b0;
        @(posedge clk);
        #1;
        check(!speaker && !cassette_out && annunciators == 4'h0 &&
              !ioudis && !keyboard_strobe && paddle_active == 4'h0,
              "Apple reset did not clear motherboard I/O state");

        $display("ONEE MOTHERBOARD IO PASS");
        $finish;
    end

    initial begin
        #500000;
        $fatal(1, "ONEE MOTHERBOARD IO TIMEOUT");
    end

endmodule
