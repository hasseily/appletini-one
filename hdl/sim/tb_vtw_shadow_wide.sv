`timescale 1ns / 1ps

// Check byte preservation, flat-map boundaries, and real memory-beat counts
// through the wide host sequencer. Do not depend on inferred array names.
module tb_vtw_shadow_wide;
    localparam int BYTES = 18'h24000;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic host_mode = 1'b0;
    logic a_en = 1'b0, a_we = 1'b0;
    logic [17:0] a_addr = '0;
    logic [7:0] a_wdata = '0;
    wire [7:0] a_rdata;
    wire [31:0] a_rdata32;
    logic raw_en = 1'b0, raw_we = 1'b0, raw_word_we = 1'b0;
    logic [17:0] raw_addr = '0;
    logic [7:0] raw_wdata = '0;
    logic [31:0] raw_wdata32 = '0;
    wire [7:0] b_rdata;
    wire [31:0] b_rdata32;

    logic addr_set = 1'b0, byte_write = 1'b0;
    logic word_write = 1'b0, word_read = 1'b0;
    logic [17:0] addr_value = '0;
    logic [7:0] byte_wdata = '0;
    logic [31:0] word_wdata = '0;
    wire [17:0] pointer, sh_addr;
    wire [7:0] read_data, sh_wdata;
    wire word_ready, word_busy, word_read_ready, word_read_busy;
    wire [29:0] word_accept_count, word_read_count;
    wire [31:0] word_read_data, sh_wdata32;
    wire sh_en, sh_we, sh_word_we;
    wire b_en = host_mode ? sh_en : raw_en;
    wire b_we = host_mode ? sh_we : raw_we;
    wire b_word_we = host_mode ? sh_word_we : raw_word_we;
    wire [17:0] b_addr = host_mode ? sh_addr : raw_addr;
    wire [7:0] b_wdata = host_mode ? sh_wdata : raw_wdata;
    wire [31:0] b_wdata32 = host_mode ? sh_wdata32 : raw_wdata32;

    logic [7:0] expected [0:BYTES-1];
    integer checks = 0;
    integer write_beats = 0, read_beats = 0, wide_beats = 0;
    logic [31:0] random_state = 32'h65813333;

    vtw_shadow shadow (
        .clk(clk), .a_en(a_en), .a_addr(a_addr), .a_we(a_we),
        .a_wdata(a_wdata), .a_rdata(a_rdata), .a_rdata32(a_rdata32),
        .b_en(b_en), .b_addr(b_addr), .b_we(b_we), .b_wdata(b_wdata),
        .b_rdata(b_rdata), .b_rdata32(b_rdata32),
        .b_word_we(b_word_we), .b_wdata32(b_wdata32)
    );
    vtw_shadow_host_port #(.WIDE_PORT(1'b1)) host (
        .clk(clk), .rstn(rstn), .addr_set(addr_set), .addr_value(addr_value),
        .byte_write(byte_write), .byte_wdata(byte_wdata),
        .word_write(word_write), .word_wdata(word_wdata), .word_read(word_read),
        .pointer(pointer), .read_data(read_data), .word_ready(word_ready),
        .word_busy(word_busy), .word_accept_count(word_accept_count),
        .word_read_data(word_read_data), .word_read_ready(word_read_ready),
        .word_read_busy(word_read_busy), .word_read_count(word_read_count),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we), .sh_wdata(sh_wdata),
        .sh_rdata(b_rdata), .sh_rdata32(b_rdata32),
        .sh_word_we(sh_word_we), .sh_wdata32(sh_wdata32)
    );

    task automatic check(input logic condition, input string reason);
        checks++;
        if (condition !== 1'b1)
            $fatal(1, "VTW SHADOW WIDE FAIL check=%0d pointer=%05x: %s",
                   checks, pointer, reason);
    endtask

    function automatic logic [31:0] next_random;
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return random_state;
    endfunction

    function automatic logic [31:0] expected_word(input integer address);
        return {expected[address+3], expected[address+2], expected[address+1], expected[address]};
    endfunction

    task automatic tick;
        #2;
        if (a_en && a_we && !a_addr[17])
            expected[a_addr] = a_wdata;
        if (b_en && b_we) begin
            if (b_word_we === 1'b1 && b_addr[1:0] == 0) begin
                for (int lane = 0; lane < 4; lane++)
                    expected[b_addr + lane] = b_wdata32[8*lane +: 8];
            end else expected[b_addr] = b_wdata;
        end
        if (host_mode && sh_en) begin
            if (sh_we) write_beats++;
            else read_beats++;
            if (sh_we && sh_word_we) wide_beats++;
        end
        #3 clk = 1'b1;
        #5 clk = 1'b0;
    endtask

    task automatic check_a(input integer address, input logic [31:0] value);
        a_en = 1'b1;
        a_we = 1'b0;
        a_addr = 18'(address);
        tick();
        check(a_rdata32 === value,
              $sformatf("A word at %05x: %08x != %08x", address, a_rdata32, value));
        check(a_rdata === value[8*(address % 4) +: 8], "A byte lane mismatch");
        a_en = 1'b0;
    endtask

    task automatic wait_idle;
        integer clocks;
        clocks = 0;
        while ((!word_read_ready || word_busy || word_read_busy) && clocks < 100) begin
            tick();
            clocks++;
        end
        check(clocks < 100, "host failed to return idle");
    endtask

    task automatic set_address(input integer address);
        addr_value = 18'(address);
        addr_set = 1'b1;
        tick();
        addr_set = 1'b0;
        wait_idle();
        check(pointer == 18'(address), "scalar address changed during prefetch");
        check(read_data === expected[address], "scalar prefetch data mismatch");
    endtask

    task automatic push_word(input logic [31:0] value);
        check(word_ready, "test attempted a write without ready");
        word_wdata = value;
        word_write = 1'b1;
        tick();
        word_write = 1'b0;
    endtask

    task automatic word_roundtrip(input integer address, input logic [31:0] value);
        integer writes_before, reads_before, wide_before;
        logic [29:0] accepts_before, count_before;
        set_address(address);
        writes_before = write_beats;
        wide_before = wide_beats;
        accepts_before = word_accept_count;
        push_word(value);
        wait_idle();
        check(pointer == 18'(address+4), "packed write pointer mismatch");
        check(word_accept_count == accepts_before+1, "packed acceptance count mismatch");
        check(write_beats-writes_before == ((address % 4 == 0) ? 1 : 4),
              "packed write used wrong number of memory beats");
        check(wide_beats-wide_before == ((address % 4 == 0) ? 1 : 0),
              "unaligned write unexpectedly switched to wide halfway through");
        for (int lane = 0; lane < 4; lane++) begin
            a_addr = 18'(address+lane);
            a_en = 1'b1;
            tick();
            check(a_rdata === value[8*lane +: 8], "packed write data/address mismatch");
        end
        a_en = 1'b0;
        set_address(address);
        reads_before = read_beats;
        count_before = word_read_count;
        word_read = 1'b1;
        tick();
        word_read = 1'b0;
        wait_idle();
        check(word_read_data === value, "packed read data mismatch");
        check(pointer == 18'(address+4), "packed read pointer mismatch");
        check(word_read_count == count_before+1, "packed read completion count mismatch");
        check(read_beats-reads_before == ((address % 4 == 0) ? 1 : 4),
              "packed read used wrong number of memory beats");
    endtask

    initial begin : test
        integer address, writes_before, wide_before;
        logic [29:0] accepts_before, reads_before;
        logic [31:0] saved_word, saved_a, saved_b;
        logic [7:0] saved_a_byte, saved_b_byte;
        tick();

        // Initialize all valid banks through the public word port so every
        // later lane-preservation check has known neighboring bytes.
        raw_en = 1'b1;
        raw_we = 1'b1;
        raw_word_we = 1'b1;
        for (address = 0; address < BYTES; address += 4) begin
            raw_addr = 18'(address);
            raw_wdata32 = next_random();
            tick();
        end
        raw_we = 1'b0;
        raw_word_we = 1'b0;

        // Read both registered ports at distinct addresses, then prove the
        // byte mux and wide output hold their prior value when enable is low.
        for (int sample = 0; sample < 256; sample++) begin
            address = int'(next_random() % BYTES);
            a_en = 1'b1;
            a_addr = 18'(address);
            raw_addr = 18'((address + 16'h1000) % BYTES);
            tick();
            check(a_rdata === expected[a_addr], "A scalar read mismatch");
            check(a_rdata32 === expected_word(address & ~3), "A aligned word mismatch");
            check(b_rdata === expected[raw_addr], "B scalar read mismatch");
            check(b_rdata32 === expected_word(int'(raw_addr) & ~3), "B aligned word mismatch");
            saved_a = a_rdata32;
            saved_b = b_rdata32;
            saved_a_byte = a_rdata;
            saved_b_byte = b_rdata;
            a_en = 1'b0;
            raw_en = 1'b0;
            a_addr = ~a_addr;
            raw_addr = ~raw_addr;
            tick();
            check(a_rdata32 === saved_a && b_rdata32 === saved_b, "disabled read output changed");
            check(a_rdata === saved_a_byte && b_rdata === saved_b_byte, "disabled byte lane changed");
            raw_en = 1'b1;
        end

        // Exercise all lanes in main, aux and ROM, including each bank end.
        for (int region = 0; region < 6; region++) begin
            case (region)
                0: address = 'h00000;
                1: address = 'h0FFFC;
                2: address = 'h10000;
                3: address = 'h1FFFC;
                4: address = 'h20000;
                default: address = 'h23FFC;
            endcase
            raw_en = 1'b0;
            for (int lane = 0; lane < 4; lane++) begin
                saved_word = expected_word(address);
                a_en = 1'b1;
                a_we = 1'b1;
                a_addr = 18'(address+lane);
                a_wdata = 8'h51 + 8'(lane);
                tick();
                a_we = 1'b0;
                if (address < 'h20000) saved_word[8*lane +: 8] = a_wdata;
                check_a(address+lane, saved_word); // ROM writes must be ignored.
                raw_en = 1'b1;
                raw_we = 1'b1;
                raw_addr = 18'(address+lane);
                raw_wdata = 8'hA1 + 8'(lane);
                raw_word_we = 1'bx; // Legacy unconnected wide-enable is scalar.
                tick();
                raw_we = 1'b0;
                raw_word_we = 1'b0;
                saved_word[8*lane +: 8] = raw_wdata;
                tick();
                check(b_rdata32 === saved_word, "B byte write changed neighboring lanes");
                raw_en = 1'b0;
                check_a(address+lane, saved_word);
            end
        end

        // The independent ports may write different banks on the same edge.
        a_en = 1'b1;
        a_we = 1'b1;
        a_addr = 18'h10011;
        a_wdata = 8'h5A;
        raw_en = 1'b1;
        raw_we = 1'b1;
        raw_word_we = 1'b1;
        raw_addr = 18'h20010;
        raw_wdata32 = 32'hDEADC0DE;
        tick();
        a_we = 1'b0;
        raw_we = 1'b0;
        raw_word_we = 1'b0;
        tick();
        check(a_rdata === 8'h5A && a_rdata32 === expected_word('h10010), "concurrent A write lost data");
        check(b_rdata32 === 32'hDEADC0DE, "concurrent B ROM write lost data");
        a_en = 1'b0;
        raw_en = 1'b0;

        host_mode = 1'b1;
        rstn = 1'b1;
        tick();
        for (int region = 0; region < 5; region++) begin
            case (region)
                0: address = 'h00100;
                1: address = 'h0FFFC;
                2: address = 'h1FFFC;
                3: address = 'h20000;
                default: address = 'h23FF8;
            endcase
            for (int lane = 0; lane < 4; lane++)
                word_roundtrip(address+lane, next_random());
        end
        word_roundtrip('h23FFC, 32'hF1F2F3F4);

        // Scalar writes still advance by one and refresh the next byte.
        set_address('h0FFFF);
        saved_word = expected_word('h0FFFC);
        writes_before = write_beats;
        wide_before = wide_beats;
        byte_wdata = 8'hD7;
        byte_write = 1'b1;
        tick();
        byte_write = 1'b0;
        wait_idle();
        check(pointer == 18'h10000, "scalar pointer did not cross main/aux boundary");
        check(read_data === expected['h10000], "scalar next-byte prefetch used wrong bank");
        check(write_beats == writes_before+1 && wide_beats == wide_before, "scalar write became wide");
        saved_word[31:24] = 8'hD7;
        check_a('h0FFFF, saved_word);

        // A full two-entry queue rejects a third command until ready returns.
        set_address('h00800);
        accepts_before = word_accept_count;
        writes_before = write_beats;
        reads_before = word_read_count;
        push_word(32'h11223344);
        push_word(32'h55667788);
        check(!word_ready && !word_read_ready, "two-entry queue did not backpressure");
        word_write = 1'b1;
        word_wdata = 32'h99AABBCC;
        word_read = 1'b1;
        tick();
        word_write = 1'b0;
        word_read = 1'b0;
        check(word_accept_count == accepts_before+2, "full queue accepted a third word");
        push_word(32'h99AABBCC);
        wait_idle();
        check(pointer == 18'h0080C && word_accept_count == accepts_before+3, "queue pointer/count mismatch");
        check(write_beats == writes_before+3, "aligned queue did not write one beat per word");
        check(word_read_count == reads_before, "busy read command unexpectedly completed");
        check_a('h00800, 32'h11223344);
        check_a('h00804, 32'h55667788);
        check_a('h00808, 32'h99AABBCC);

        // Cancellation lets the presented word land, discards the queued
        // second word, and performs the new address's scalar prefetch.
        set_address('h00900);
        saved_word = expected_word('h00904);
        writes_before = write_beats;
        push_word(32'h01234567);
        push_word(32'hBAD0BAD0);
        addr_set = 1'b1;
        addr_value = 18'h00A00;
        tick();
        addr_set = 1'b0;
        wait_idle();
        repeat (5) tick();
        check(pointer == 18'h00A00 && read_data === expected['h00A00], "address cancellation lost pointer/prefetch");
        check(write_beats == writes_before+1, "canceled queue emitted another write");
        check_a('h00900, 32'h01234567);
        check_a('h00904, saved_word);

        // A scalar override follows the already-presented wide beat at the
        // advanced pointer, and cancels the remainder of the packed queue.
        set_address('h00B00);
        saved_word = expected_word('h00B04);
        writes_before = write_beats;
        push_word(32'h10203040);
        push_word(32'hBAD0BAD0);
        byte_write = 1'b1;
        byte_wdata = 8'hE7;
        tick();
        byte_write = 1'b0;
        wait_idle();
        check(pointer == 18'h00B05 && write_beats == writes_before+2, "scalar cancellation pointer/beats mismatch");
        check_a('h00B00, 32'h10203040);
        saved_word[7:0] = 8'hE7;
        check_a('h00B04, saved_word);

        // Address-setting outranks simultaneous byte/word requests.
        accepts_before = word_accept_count;
        writes_before = write_beats;
        addr_value = 18'h00100;
        addr_set = 1'b1;
        byte_write = 1'b1;
        word_write = 1'b1;
        tick();
        addr_set = 1'b0;
        byte_write = 1'b0;
        word_write = 1'b0;
        wait_idle();
        check(pointer == 18'h00100 && word_accept_count == accepts_before &&
              write_beats == writes_before, "address priority accepted a canceled write");

        // Cancel an unaligned read before capture; it must never later count
        // as a completed packed read or advance the replacement pointer.
        set_address('h0FFFF);
        reads_before = word_read_count;
        word_read = 1'b1;
        tick();
        word_read = 1'b0;
        addr_value = 18'h00500;
        addr_set = 1'b1;
        tick();
        addr_set = 1'b0;
        wait_idle();
        repeat (5) tick();
        check(word_read_count == reads_before && pointer == 18'h00500 &&
              read_data === expected['h00500], "read cancellation left stale work");

        $display("VTW SHADOW WIDE PASS checks=%0d writes=%0d reads=%0d wide_writes=%0d",
                 checks, write_beats, read_beats, wide_beats);
        $finish;
    end
endmodule
