`timescale 1ns / 1ps

// Real shadow BRAM, registered PSRAM acceptance, and delayed line responses.
// The golden result is a byte copy/fill from the initial fixture, independent
// of the engine's word selection, line buffer, and state machine.
module tb_vtw_copy_engine #(parameter bit RUN_TESTS = 1'b1);
    localparam int FIXTURE_BYTES = 'h22000;
    logic clk = 1'b0, rstn = 1'b0;
    logic start = 1'b0, abort_req = 1'b0, permit = 1'b1;
    logic [23:0] source = '0, destination = '0;
    logic [15:0] length = '0;
    logic copy_rows = 1'b0;
    logic [7:0] rows_minus1 = 0, source_gap = 0, destination_gap = 0;
    logic fill = 1'b0;
    logic [7:0] fill_data = 8'hA6;
    logic publish = 1'b0, publish_active = 1'b1, publish_ready = 1'b1;
    wire publish_valid;
    wire [16:0] publish_addr;
    wire [7:0] publish_data;
    wire busy, done, error, aborted;
    wire [15:0] completed;
    wire sh_en, sh_we, sh_word_we;
    wire [17:0] sh_addr;
    wire [31:0] sh_wdata, sh_rdata;
    wire ps_valid, ps_rw;
    wire [23:0] ps_addr;
    wire [63:0] ps_wdata;
    logic ps_ready = 1'b0, ps_rvalid = 1'b0;
    logic [63:0] ps_rdata = '0;

    logic [7:0] ps_memory [0:FIXTURE_BYTES-1];
    logic [7:0] expected [0:FIXTURE_BYTES-1];
    logic pending = 1'b0, accepted_rw = 1'b1;
    logic [23:0] accepted_addr = '0;
    logic [63:0] accepted_data = '0;
    logic block_requests = 1'b0;
    integer response_delay = 0, admission_delay = 0;
    integer clocks = 0, checks = 0, cases = 0;
    integer ps_reads = 0, ps_writes = 0, sh_reads = 0;
    integer sh_writes = 0, sh_wide_writes = 0, written_bytes = 0;
    integer command_src = 0, command_dst = 0, command_len = 0, command_width = 1;
    logic command_fill = 1'b0, track_progress = 1'b0;
    logic [7:0] command_fill_data;
    string label = "reset";

    vtw_copy_engine dut (.*);
    vtw_shadow shadow (
        .clk(clk), .a_en(1'b0), .a_addr(18'd0), .a_we(1'b0),
        .a_wdata(8'd0), .a_rdata(), .a_rdata32(),
        .b_en(sh_en), .b_addr(sh_addr), .b_we(sh_we),
        .b_wdata(sh_wdata[7:0]), .b_rdata(), .b_rdata32(sh_rdata),
        .b_word_we(sh_word_we), .b_wdata32(sh_wdata)
    );

    initial begin
        #10000000;
        $fatal(1, "VTW COPY ENGINE FAIL bench watchdog expired: %s", label);
    end

    task automatic check(input logic condition, input string reason);
        checks++;
        if (condition !== 1'b1)
            $fatal(1, "VTW COPY ENGINE FAIL %s clock=%0d completed=%0d: %s",
                   label, clocks, completed, reason);
    endtask

    function automatic logic [7:0] initial_byte(input integer address);
        return 8'((address * 73) ^ (address >> 8) ^ (address >> 16) ^ 'h59);
    endfunction

    function automatic logic [7:0] actual_byte(input integer address);
        if (address < 'h10000)
            return shadow.mem_main[address >> 2][8*(address & 3) +: 8];
        if (address < 'h20000)
            return shadow.mem_aux[(address - 'h10000) >> 2][8*(address & 3) +: 8];
        return ps_memory[address];
    endfunction

    // ps_ready is the controller's registered acceptance pulse. The request
    // has already been captured when the DUT sees it on the following edge.
    // Responses likewise arrive only after acceptance, with varying latency.
    function automatic integer source_at(input integer index);
        return command_src + index + (copy_rows ? (index/command_width)*int'(source_gap) : 0);
    endfunction
    function automatic integer destination_at(input integer index);
        return command_dst + index + (copy_rows ? (index/command_width)*int'(destination_gap) : 0);
    endfunction

    task automatic tick;
        logic next_ready, next_rvalid;
        logic [63:0] next_rdata;
        integer commit_bytes;
        #2;
        clocks++;
        next_ready = 1'b0;
        next_rvalid = 1'b0;
        next_rdata = ps_rdata;
        if (rstn && sh_en) begin
            check(sh_addr < 'h20000, "shadow access escaped RAM banks");
            if (sh_we) begin
                commit_bytes = sh_word_we ? 4 : 1;
                check(!sh_word_we || sh_addr[1:0] == 0, "unaligned shadow word write");
                check(sh_addr == destination_at(written_bytes), "shadow writes are not an ordered prefix");
                check(sh_addr + commit_bytes <= destination_at(command_len-1)+1, "shadow write exceeded length");
                written_bytes += commit_bytes;
                sh_writes++;
                if (sh_word_we) sh_wide_writes++;
            end else sh_reads++;
        end
        if (ps_rvalid && !accepted_rw) begin
            commit_bytes = 0;
            for (int lane = 0; lane < 8; lane++) begin
                ps_memory[accepted_addr + lane] = accepted_data[8*lane +: 8];
                for (int index = written_bytes; index < command_len; index++)
                    if (destination_at(index) == accepted_addr + lane) commit_bytes++;
            end
            check(accepted_addr <= destination_at(written_bytes) &&
                  accepted_addr + 8 > destination_at(written_bytes),
                  "PSRAM writes are not an ordered prefix");
            written_bytes += commit_bytes;
        end
        if (pending) begin
            if (response_delay == 0) begin
                for (int lane = 0; lane < 8; lane++)
                    next_rdata[8*lane +: 8] = ps_memory[accepted_addr + lane];
                next_rvalid = 1'b1;
                pending = 1'b0;
            end else response_delay--;
        end else if (!ps_ready && !ps_rvalid && ps_valid && !block_requests) begin
            if (admission_delay == 0) begin
                check(ps_addr[2:0] == 0 && ps_addr >= 'h20000 &&
                      ps_addr + 8 <= FIXTURE_BYTES, "bad PSRAM line address");
                accepted_addr = ps_addr;
                accepted_data = ps_wdata;
                accepted_rw = ps_rw;
                pending = 1'b1;
                next_ready = 1'b1;
                response_delay = 3 + clocks % 7;
                admission_delay = 1 + clocks % 4;
                if (ps_rw) ps_reads++;
                else ps_writes++;
            end else admission_delay--;
        end
        #3 clk = 1'b1;
        #1;
        ps_ready = next_ready;
        ps_rvalid = next_rvalid;
        ps_rdata = next_rdata;
        if (rstn && track_progress)
            check(completed == written_bytes, "progress differs from committed destination bytes");
        #4 clk = 1'b0;
    endtask

    task automatic fixture;
        check(!busy && !pending && !ps_rvalid && !ps_ready, "previous command did not drain");
        permit = 1'b1;
        abort_req = 1'b0;
        block_requests = 1'b0;
        track_progress = 1'b0;
        for (int address = 0; address < FIXTURE_BYTES; address++) begin
            expected[address] = initial_byte(address);
            if (address < 'h10000)
                shadow.mem_main[address >> 2][8*(address & 3) +: 8] = initial_byte(address);
            else if (address < 'h20000)
                shadow.mem_aux[(address - 'h10000) >> 2][8*(address & 3) +: 8] = initial_byte(address);
            else ps_memory[address] = initial_byte(address);
        end
        ps_reads = 0;
        ps_writes = 0;
        sh_reads = 0;
        sh_writes = 0;
        sh_wide_writes = 0;
        written_bytes = 0;
        admission_delay = 3;
    endtask

    task automatic launch(input integer src, dst, count, input logic is_fill);
        command_src = src;
        command_dst = dst;
        command_width = count == 0 ? 1 : count;
        command_len = copy_rows ? count*(int'(rows_minus1)+1) : count;
        command_fill = is_fill;
        command_fill_data = fill_data;
        source = 24'(src);
        destination = 24'(dst);
        length = 16'(count);
        fill = is_fill;
        start = 1'b1;
        tick();
        start = 1'b0;
        track_progress = 1'b1;
        check(completed == 0, "new command did not clear progress");
        if (permit && !abort_req && (!publish || publish_active))
            check(busy && !done && !error && !aborted, "new command did not clear sticky status");
    endtask

    task automatic wait_done;
        integer budget;
        budget = 200000;
        while (busy && budget > 0) begin
            tick();
            budget--;
        end
        check(budget > 0 && done, "command timed out");
        check(!pending && !ps_rvalid && !ps_ready, "completion preceded PSRAM drain");
    endtask

    task automatic verify_memory;
        for (int index = 0; index < written_bytes; index++)
            expected[destination_at(index)] = command_fill ? command_fill_data :
                                           initial_byte(source_at(index));
        for (int address = 0; address < FIXTURE_BYTES; address++) begin
            if (actual_byte(address) !== expected[address])
                $fatal(1, "VTW COPY ENGINE FAIL %s memory[%06x]=%02x expected=%02x",
                       label, address, actual_byte(address), expected[address]);
        end
        checks++;
    endtask

    task automatic sticky;
        logic saved_error, saved_aborted;
        integer saved_completed, saved_writes;
        saved_error = error;
        saved_aborted = aborted;
        saved_completed = completed;
        saved_writes = ps_writes + sh_writes;
        repeat (8) begin
            tick();
            check(done && !busy && error == saved_error && aborted == saved_aborted &&
                  completed == saved_completed, "terminal status is not sticky");
        end
        check(saved_writes == ps_writes + sh_writes, "write after terminal status");
        cases++;
    endtask

    task automatic success(input integer src, dst, count, input logic is_fill);
        fixture();
        launch(src, dst, count, is_fill);
        wait_done();
        check(!error && !aborted && completed == count, "successful command status");
        verify_memory();
        sticky();
    endtask

    task automatic invalid(input integer src, dst, count, input logic is_fill);
        fixture();
        launch(src, dst, count, is_fill);
        wait_done();
        check(error && !aborted && completed == 0, "invalid descriptor status");
        check(ps_reads + ps_writes + sh_reads + sh_writes == 0,
              "invalid descriptor touched memory");
        verify_memory();
        sticky();
    endtask

    task automatic await_request(input logic rw, input logic accepted);
        integer budget;
        budget = 10000;
        while (!(accepted ? (ps_ready && accepted_rw == rw) : (ps_valid && ps_rw == rw)) && budget > 0) begin
            tick();
            budget--;
        end
        check(budget > 0, "expected PSRAM request did not arrive");
    endtask

    task automatic cancel_ps(input logic rw, accepted, lose_permit, input integer wait_cycles);
        integer saved_requests, expected_progress;
        fixture();
        if (!accepted) block_requests = 1'b1;
        // A full destination line needs no read before its write request.
        launch(rw ? 'h20200 : 'h1000, rw ? 'h11000 : 'h21000, 40, 1'b0);
        await_request(rw, accepted);
        if (wait_cycles < 0) begin
            while (!ps_rvalid) tick();
        end else repeat (wait_cycles) tick();
        saved_requests = ps_reads + ps_writes;
        expected_progress = accepted && !rw ? 8 : 0;
        if (lose_permit) permit = 1'b0;
        else abort_req = 1'b1;
        tick();
        if (accepted && wait_cycles >= 0)
            check(busy && !done, "cancel did not wait for accepted PSRAM response");
        // Restoring inputs must not revoke a latched cancellation.
        permit = 1'b1;
        abort_req = 1'b0;
        block_requests = 1'b0;
        wait_done();
        check(aborted && !error && completed == expected_progress, "cancelled PSRAM progress/status");
        check(ps_reads + ps_writes == saved_requests, "cancel issued another PSRAM request");
        verify_memory();
        sticky();
    endtask

    initial if (RUN_TESTS) begin
        repeat (3) tick();
        rstn = 1'b1;
        tick();
        check(!busy && !done && !error && !aborted && completed == 0, "reset status");

        for (int pairing = 0; pairing < 4; pairing++) begin
            integer src, dst;
            src = pairing[1] ? 'h20200 : 'h1000;
            dst = pairing[0] ? 'h21000 : 'h11000;
            label = $sformatf("aligned pairing %0d", pairing);
            success(src, dst, 64, 1'b0);
            check(ps_reads == (pairing[1] ? 8 : 0), "aligned copy did extra PSRAM reads");
            check(ps_writes == (pairing[0] ? 8 : 0), "aligned copy did wrong PSRAM writes");
            if (!pairing[0]) check(sh_wide_writes == 16, "aligned shadow copy did not use wide writes");
            for (int src_offset = 0; src_offset < 4; src_offset++) begin
                for (int dst_offset = 0; dst_offset < 4; dst_offset++) begin
                    label = $sformatf("unaligned pairing %0d src+%0d dst+%0d", pairing, src_offset, dst_offset);
                    success(src + src_offset, dst + dst_offset, 37, 1'b0);
                end
            end
        end

        label = "copy exceeds one DMA transaction";
        success('h20203, 'h11001, 1537, 1'b0);
        label = "copy to PSRAM exceeds one DMA transaction";
        success('h1000, 'h21000, 1536, 1'b0);
        label = "source crosses shadow to PSRAM";
        success('h1FFFB, 'h11001, 41, 1'b0);
        label = "destination crosses shadow to PSRAM";
        success('h1001, 'h1FFFB, 41, 1'b0);
        label = "fill shadow wide";
        success('hFFFFFF, 'h11000, 1024, 1'b1);
        check(sh_wide_writes == 256 && sh_reads == 0, "fill shadow did not use wide writes");
        label = "fill shadow unaligned";
        success(0, 'h11003, 19, 1'b1);
        label = "fill PSRAM wide";
        success(0, 'h21000, 1024, 1'b1);
        check(ps_reads == 0 && ps_writes == 128, "full-line fill did unnecessary read-modify-write");
        label = "fill PSRAM partial lines";
        success(0, 'h21003, 19, 1'b1);
        check(ps_reads == 2 && ps_writes == 3, "partial fill did not preserve both line boundaries");
        label = "one-byte PSRAM copy";
        success('h20207, 'h21007, 1, 1'b0);

        label = "zero length";
        invalid('h1000, 'h11000, 0, 1'b0);
        label = "overlap forward";
        invalid('h1000, 'h1004, 16, 1'b0);
        label = "overlap backward";
        invalid('h1004, 'h1000, 16, 1'b0);
        label = "same source and destination";
        invalid('h20200, 'h20200, 16, 1'b0);
        label = "source out of range";
        invalid('h800000, 'h11000, 16, 1'b0);
        label = "destination out of range";
        invalid('h1000, 'h800000, 16, 1'b0);
        label = "source end overflow";
        invalid('h7FFFF8, 'h11000, 16, 1'b0);
        label = "destination end overflow";
        invalid('h1000, 'h7FFFF8, 16, 1'b0);
        label = "fill end overflow";
        invalid(0, 'h7FFFF8, 16, 1'b1);
        label = "missing permit at start";
        fixture();
        permit = 1'b0;
        launch('h1000, 'h11000, 16, 1'b0);
        wait_done();
        check(error && !aborted && completed == 0 && !sh_en && !ps_valid,
              "missing permit was not rejected");
        verify_memory();
        sticky();

        for (int lose = 0; lose < 2; lose++) begin
            label = $sformatf("cancel unaccepted read permit-loss=%0d", lose);
            cancel_ps(1'b1, 1'b0, 1'(lose), 0);
            label = $sformatf("cancel accepted read permit-loss=%0d", lose);
            cancel_ps(1'b1, 1'b1, 1'(lose), 0);
            label = $sformatf("cancel unaccepted write permit-loss=%0d", lose);
            cancel_ps(1'b0, 1'b0, 1'(lose), 0);
            label = $sformatf("cancel accepted write permit-loss=%0d", lose);
            cancel_ps(1'b0, 1'b1, 1'(lose), 0);
            label = $sformatf("cancel read in flight permit-loss=%0d", lose);
            cancel_ps(1'b1, 1'b1, 1'(lose), 2);
            label = $sformatf("cancel write in flight permit-loss=%0d", lose);
            cancel_ps(1'b0, 1'b1, 1'(lose), 2);
            label = $sformatf("cancel on read response permit-loss=%0d", lose);
            cancel_ps(1'b1, 1'b1, 1'(lose), -1);
            label = $sformatf("cancel on write response permit-loss=%0d", lose);
            cancel_ps(1'b0, 1'b1, 1'(lose), -1);
        end

        label = "abort before any request";
        fixture();
        launch('h20200, 'h11000, 40, 1'b0);
        abort_req = 1'b1;
        tick();
        abort_req = 1'b0;
        wait_done();
        check(aborted && completed == 0 && ps_reads + ps_writes + sh_reads + sh_writes == 0,
              "early abort touched memory");
        verify_memory();
        sticky();

        label = "abort discards partial buffer after committed line";
        fixture();
        launch('h1000, 'h21000, 13, 1'b0);
        while (completed == 0) tick();
        // The last partial destination line first needs its preserved bytes.
        await_request(1'b1, 1'b1);
        while (pending || ps_ready || ps_rvalid) tick();
        block_requests = 1'b1;
        await_request(1'b0, 1'b0);
        abort_req = 1'b1;
        tick();
        abort_req = 1'b0;
        block_requests = 1'b0;
        wait_done();
        check(aborted && completed == 8 && ps_writes == 1,
              "aborted partial buffer counted uncommitted bytes");
        verify_memory();
        sticky();

        label = "permit loss drains second accepted destination line";
        fixture();
        launch('h1000, 'h21000, 40, 1'b0);
        while (completed == 0) tick();
        await_request(1'b0, 1'b1);
        permit = 1'b0;
        tick();
        permit = 1'b1;
        wait_done();
        check(aborted && completed == 16 && ps_writes == 2,
              "second accepted line did not drain with exact progress");
        verify_memory();
        sticky();

        label = "permit loss before a shadow write";
        fixture();
        launch('h1000, 'h11000, 40, 1'b0);
        while (!(sh_en && sh_we)) tick();
        permit = 1'b0;
        tick();
        permit = 1'b1;
        wait_done();
        check(aborted && completed == 0 && sh_writes == 0, "permit loss allowed shadow write");
        verify_memory();
        sticky();

        label = "success after sticky abort";
        success('h1003, 'h21001, 17, 1'b0);
        label = "descriptor and start changes while busy";
        fixture();
        launch('h1000, 'h11000, 32, 1'b0);
        source = 'h20200;
        destination = 'h21000;
        length = 64;
        fill = 1'b1;
        fill_data = 8'h42;
        start = 1'b1;
        tick();
        start = 1'b0;
        wait_done();
        check(!error && !aborted && completed == 32, "busy start replaced the command");
        verify_memory();
        sticky();
        label = "reset clears sticky status";
        track_progress = 1'b0;
        rstn = 1'b0;
        tick();
        rstn = 1'b1;
        tick();
        check(!done && !error && !aborted && !busy && completed == 0, "reset retained terminal status");
        $display("VTW COPY ENGINE PASS cases=%0d checks=%0d clocks=%0d", cases, checks, clocks);
        $finish;
    end
endmodule
