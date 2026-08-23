`timescale 1ns / 1ps

// Unit bench for mouse_card: the AppleMouse-compat fix batch.
//   1. mode $08 (VBL interrupts, mouse OFF) asserts IRQ on vblank -- the
//      enable bit must not gate the VBL source; ACK $02 releases it
//   2. mode $04 (no VBL bit) must NOT assert on vblank; mode $09 must
//   3. clamp registers carry full 16-bit values; CMD $02 applies the
//      AppleWin min>max normalization ($FFEC/$012B -> $0000/$0117)
//   4. CMD $03 (INITMOUSE) restores the 0..1023 defaults
//   5. positions are 16-bit and clamp against the active window
//   6. clamp normalization and application are split across two fabric
//      clocks without changing the command-time result
//   7. SERVEMOUSE IRQ release and READMOUSE status clear are independent,
//      preserving an IRQ which arrives between the two calls
//   8. VBL-only mode records movement-since-read without reporting a
//      movement interrupt reason
module tb_mouse_card;
    logic clk = 0;
    always #3.75 clk = ~clk;

    logic rstn = 0;
    logic vbl = 0;
    globals::AppleBus_read   ab_read;
    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    globals::AppleBus_write  ab_write;
    AxiSimple_if asif();

    mouse_card dut (
        .clk(clk), .rstn(rstn),
        .vblank_start_pulse(vbl),
        .ab_read(ab_read), .sss(sss),
        .slot_assign(3'h2),
        .as_common(as_common), .as_client(asif),
        .ab_write(ab_write),
        .dbg_mode(), .dbg_vbl_pending(), .dbg_irq_pending()
    );

    int errors = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            errors++;
            $display("FAIL: %s", msg);
        end
    endtask

    // One register write as the card sees it (data_en-keyed).
    task automatic io_write(input logic [3:0] r, input logic [7:0] d);
        @(posedge clk);
        ab_read.addr    = {8'hC0, 4'hA, r};
        ab_read.rw      = 1'b0;
        ab_read.data    = d;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw      = 1'b1;
        @(posedge clk);
        // The card applies the captured write on this edge; return only after
        // its nonblocking assignments have been visible for a full clock.
        @(posedge clk);
    endtask

    // One register read (serve_en-keyed; response registers next clock).
    logic [7:0] rd;
    task automatic io_read(input logic [3:0] r);
        @(posedge clk);
        ab_read.addr     = {8'hC0, 4'hA, r};
        ab_read.rw       = 1'b1;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        ab_read.serve_en = 1'b0;
        @(posedge clk);
        rd = ab_write.wr_data;
        @(posedge clk);
    endtask

    task automatic vbl_pulse;
        @(posedge clk);
        vbl = 1'b1;
        @(posedge clk);
        vbl = 1'b0;
        repeat (2) @(posedge clk);
    endtask

    task automatic axi_write(input logic [7:0] r, input logic [31:0] d);
        @(posedge clk);
        as_common.awaddr = r;
        as_common.wdata  = d;
        as_common.wstrb  = 4'hF;
        asif.awvalid     = 1'b1;
        @(posedge clk);
        asif.awvalid     = 1'b0;
        @(posedge clk);
    endtask

    initial begin
        ab_read   = '0;
        ab_read.rw = 1'b1;
        sss       = '0;
        sss.slot_access = 1'b1;
        as_common = '0;
        asif.awvalid = 1'b0;
        repeat (4) @(posedge clk);
        rstn = 1;
        ab_read.res = 1'b1;
        repeat (4) @(posedge clk);

        // --- 1. mode $08: VBL interrupt with the mouse disabled ---------
        io_write(4'hE, 8'h08);
        vbl_pulse();
        check(ab_write.assert_irq === 1'b1,
              "mode $08 vblank did not assert IRQ (enable bit gated VBL)");
        io_write(4'hF, 8'h02);          // ACK: clear interrupt latch
        @(posedge clk);
        check(ab_write.assert_irq === 1'b0, "ACK $02 did not release IRQ");
        io_read(4'h0);
        check((rd & 8'h08) == 8'h08,
              $sformatf("SERVEMOUSE cleared VBL cause: %02X", rd));
        io_write(4'hF, 8'h01);          // READMOUSE: clear status causes
        io_read(4'h0);
        check((rd & 8'h0E) == 8'h00,
              $sformatf("READMOUSE left cause bits set: %02X", rd));

        // --- 2. no VBL mode bit: vblank must stay silent ----------------
        io_write(4'hE, 8'h04);
        vbl_pulse();
        check(ab_write.assert_irq === 1'b0, "mode $04 vblank asserted IRQ");
        io_write(4'hE, 8'h09);
        vbl_pulse();
        check(ab_write.assert_irq === 1'b1, "mode $09 vblank did not assert");

        // Original-card race: SERVEMOUSE releases this IRQ, then another
        // VBL arrives before READMOUSE clears status. READMOUSE must not
        // release that second IRQ; the following SERVEMOUSE does so even
        // though its status reason is now empty.
        io_write(4'hF, 8'h02);          // SERVEMOUSE
        @(posedge clk);
        check(ab_write.assert_irq === 1'b0, "SERVEMOUSE did not release IRQ");
        io_read(4'h0);
        check((rd & 8'h08) == 8'h08,
              $sformatf("SERVEMOUSE consumed first VBL cause: %02X", rd));
        vbl_pulse();                    // IRQ after SERVEMOUSE
        check(ab_write.assert_irq === 1'b1, "second VBL did not assert IRQ");
        io_write(4'hF, 8'h01);          // READMOUSE clears reasons only
        check(ab_write.assert_irq === 1'b1,
              "READMOUSE swallowed IRQ asserted after SERVEMOUSE");
        io_read(4'h0);
        check((rd & 8'h0E) == 8'h00,
              $sformatf("READMOUSE did not clear reason bits: %02X", rd));
        io_write(4'hF, 8'h02);          // spurious SERVEMOUSE still releases
        @(posedge clk);
        check(ab_write.assert_irq === 1'b0,
              "reasonless SERVEMOUSE did not release IRQ");

        // VBL-only mode $09: movement is visible in bit 5 for READMOUSE,
        // but must not invent bit 1 or assert IRQ.
        axi_write(8'h00, 32'h0000_0001); // USB mouse connected
        axi_write(8'h01, 32'h0000_0010); // X changed
        axi_write(8'h04, 32'h0000_0001); // commit
        io_read(4'h0);
        check((rd & 8'h22) == 8'h20,
              $sformatf("VBL-only movement status wrong: %02X", rd));
        check(ab_write.assert_irq === 1'b0,
              "VBL-only movement incorrectly asserted IRQ");
        io_write(4'hF, 8'h01);

        // Enabling movement IRQ makes the same position change set both
        // movement bits and assert the line.
        io_write(4'hE, 8'h03);
        axi_write(8'h01, 32'h0000_0020);
        axi_write(8'h04, 32'h0000_0002);
        io_read(4'h0);
        check((rd & 8'h22) == 8'h22,
              $sformatf("movement IRQ status wrong: %02X", rd));
        check(ab_write.assert_irq === 1'b1,
              "enabled movement did not assert IRQ");
        io_write(4'hF, 8'h02);
        io_write(4'hF, 8'h01);
        io_write(4'hE, 8'h00);

        // --- 3. 16-bit clamps + AppleWin min>max normalization ----------
        io_write(4'h7, 8'h00);          // axis X
        io_write(4'h8, 8'hEC);          // min lo  ($FFEC)
        io_write(4'h9, 8'hFF);          // min hi
        io_write(4'hA, 8'h2B);          // max lo  ($012B)
        io_write(4'hB, 8'h01);          // max hi
        io_write(4'hC, 8'h02);          // commit clamp
        io_read(4'h8); check(rd === 8'h00, $sformatf("norm min lo=%02X", rd));
        io_read(4'h9); check(rd === 8'h00, $sformatf("norm min hi=%02X", rd));
        io_read(4'hA); check(rd === 8'h17, $sformatf("norm max lo=%02X", rd));
        io_read(4'hB); check(rd === 8'h01, $sformatf("norm max hi=%02X", rd));

        // Absolute position writes store RAW (manual: CLEARMOUSE must be
        // able to set 0,0 under any clamp window); only a clamp COMMIT
        // re-clamps the position into the window ($0117 = 279).
        io_write(4'h1, 8'h34);          // X lo
        io_write(4'h2, 8'h02);          // X hi -> $0234 = 564, stored raw
        io_read(4'h1); check(rd === 8'h34, $sformatf("raw X lo=%02X", rd));
        io_read(4'h2); check(rd === 8'h02, $sformatf("raw X hi=%02X", rd));
        // Drive this command explicitly to verify the timing split: the
        // command edge captures the old position and normalized bounds, and
        // the next 133 MHz edge applies the clamp.
        @(negedge clk);
        ab_read.addr    = {8'hC0, 4'hA, 4'hC};
        ab_read.rw      = 1'b0;
        ab_read.data    = 8'h02;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        check(dut.apple_write_pending_q === 1'b1,
              "clamp command did not enter the Apple write stage");
        check(dut.clamp_apply_pending_q === 1'b0,
              "clamp command bypassed the Apple write stage");
        check(dut.mouse_x_q === 16'h0234,
              "clamp changed X on the capture edge");
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw      = 1'b1;
        @(posedge clk);
        #1;
        check(dut.clamp_apply_pending_q === 1'b1,
              "staged clamp command did not arm the apply stage");
        check(dut.mouse_x_q === 16'h0234,
              "clamp changed X on the normalization edge");
        @(posedge clk);
        #1;
        check(dut.clamp_apply_pending_q === 1'b0,
              "clamp apply stage did not retire");
        check(dut.mouse_x_q === 16'h0117,
              $sformatf("staged clamp X=%04X", dut.mouse_x_q));
        io_read(4'h1); check(rd === 8'h17, $sformatf("committed X lo=%02X", rd));
        io_read(4'h2); check(rd === 8'h01, $sformatf("committed X hi=%02X", rd));

        // --- 4. CMD $03: defaults restored ------------------------------
        io_write(4'hC, 8'h03);
        io_read(4'h8); check(rd === 8'h00, "default min lo");
        io_read(4'h9); check(rd === 8'h00, "default min hi");
        io_read(4'hA); check(rd === 8'hFF, "default max lo");
        io_read(4'hB); check(rd === 8'h03, "default max hi");

        // --- 5. nonzero clamp minimum: CLEARMOUSE-style zero sticks -----
        io_write(4'h7, 8'h00);          // axis X window 5..100
        io_write(4'h8, 8'h05);
        io_write(4'h9, 8'h00);
        io_write(4'hA, 8'h64);
        io_write(4'hB, 8'h00);
        io_write(4'hC, 8'h02);
        io_write(4'h1, 8'h00);          // position := 0,0 (below min)
        io_write(4'h2, 8'h00);
        io_read(4'h1); check(rd === 8'h00, "CLEARMOUSE zero overridden by clamp min (lo)");
        io_read(4'h2); check(rd === 8'h00, "CLEARMOUSE zero overridden by clamp min (hi)");
        io_write(4'hC, 8'h03);          // restore defaults

        if (errors == 0) $display("MOUSE CARD PASS");
        else begin
            $display("tb_mouse_card: %0d FAILURES", errors);
            $fatal(1, "tb_mouse_card failed");
        end
        $finish;
    end

    initial begin
        #500us;
        $fatal(1, "tb_mouse_card: timeout");
    end
endmodule
