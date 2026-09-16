`timescale 1ns / 1ps

// Exercise the production sample player and DDR fetcher at their public
// boundaries. Address-derived PCM makes skipped, repeated, stale, or remapped
// samples visible without depending on the sound recordings' quiet passages.
module tb_disk2_sound;
    import disk2_sound_pkg::*;

    localparam logic [31:0] SAMPLE_BASE = 32'h0100_0000;
    localparam int TICK_CLOCKS = 256;
    localparam int SEEK_HOLD_SAMPLES = 1200; // 25 ms at 48 kHz
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic rstn = 1'b0, enable = 1'b1, audio_tick = 1'b0;
    logic [3:0] volume = 4'd10, sound_event = 4'd0;
    logic [31:0] sample_base_addr = SAMPLE_BASE;
    logic drive_spinning = 1'b0;
    logic [7:0] qtrack = 8'd0, seek_start_qtrack = 8'd0, seek_distance = 8'd0;
    logic signed [15:0] audio_l, audio_r;
    Axi3_read_if #(.DATA_WIDTH(64)) sample_read();

    disk2_sound_player dut (.*);

    int sample_checks = 0, request_count = 0, response_delay = 0;
    int ar_wait = 0, response_wait = 0;
    bit delayed_memory = 0, hold_response = 0, force_ar_stall = 0;
    bit response_pending = 0, previous_ar_stall = 0;
    logic [31:0] pending_address, previous_araddr;
    logic [63:0] pending_data;

    // Even, nonzero samples survive the existing event /2 and volume x2
    // exactly. The odd multiplier changes successive samples and all lanes.
    function automatic logic signed [15:0] pcm(input int sample_address);
        if (sample_address >= disk2_sound_offset(DISK2_SOUND_IDLE_SPIN) &&
            sample_address < disk2_sound_offset(DISK2_SOUND_SEEK_34_0))
            pcm = 0;
        else
            pcm = 2 * (1 + ((sample_address * 37) % 14983));
    endfunction

    function automatic logic [63:0] beat(input logic [31:0] byte_address);
        int first_sample;
        begin
            first_sample = (byte_address - SAMPLE_BASE) / 2;
            beat = {pcm(first_sample + 3), pcm(first_sample + 2),
                    pcm(first_sample + 1), pcm(first_sample)};
        end
    endfunction

    assign sample_read.arready = !response_pending && !sample_read.rvalid &&
                                 !force_ar_stall && (ar_wait == 0);
    always @(posedge clk) begin
        if (!rstn || !enable || volume == 0 || sample_base_addr == 0) begin
            sample_read.rvalid <= 0;
            sample_read.rdata <= 0;
            sample_read.rlast <= 1;
            sample_read.rresp <= 0;
            response_pending <= 0;
            ar_wait <= 0;
            response_wait <= 0;
            previous_ar_stall <= 0;
        end else begin
            if (previous_ar_stall &&
                (!sample_read.arvalid || sample_read.araddr !== previous_araddr))
                $fatal(1, "AXI address changed while ARREADY was low");
            previous_ar_stall <= sample_read.arvalid && !sample_read.arready;
            previous_araddr <= sample_read.araddr;
            if (ar_wait > 0)
                ar_wait <= ar_wait - 1;
            if (sample_read.arvalid && sample_read.arready) begin
                if (sample_read.araddr < SAMPLE_BASE ||
                    sample_read.araddr >= SAMPLE_BASE + DISK2_SOUND_SAMPLE_COUNT * 2 ||
                    sample_read.araddr[2:0] != 0 || sample_read.arlen != 0 ||
                    sample_read.arsize != 3 || sample_read.arburst != 1)
                    $fatal(1, "invalid PCM read at %h", sample_read.araddr);
                pending_address <= sample_read.araddr;
                pending_data <= beat(sample_read.araddr);
                response_pending <= 1;
                response_wait <= delayed_memory ? 11 + (request_count % 23) : 2;
                request_count <= request_count + 1;
            end
            if (response_pending && !hold_response) begin
                if (response_wait == 0) begin
                    sample_read.rdata <= pending_data;
                    sample_read.rvalid <= 1;
                    response_pending <= 0;
                end else begin
                    response_wait <= response_wait - 1;
                end
            end
            if (sample_read.rvalid && sample_read.rready) begin
                sample_read.rvalid <= 0;
                ar_wait <= delayed_memory ? request_count % 7 : 0;
            end
        end
    end

    task automatic reset_player;
        @(negedge clk);
        rstn = 0; enable = 1; volume = 10; sample_base_addr = SAMPLE_BASE;
        audio_tick = 0; sound_event = 0; drive_spinning = 1;
        hold_response = 0; force_ar_stall = 0;
        repeat (8) @(negedge clk);
        rstn = 1;
        repeat (8) @(negedge clk);
        if (audio_l !== 0 || audio_r !== 0)
            $fatal(1, "reset did not silence both channels");
    endtask

    task automatic event_pulse(input int code, input int start_track, input int distance);
        @(negedge clk);
        sound_event = code; seek_start_qtrack = start_track; seek_distance = distance;
        @(negedge clk);
        sound_event = 0;
    endtask

    task automatic ready_event(input int code, input int start_track, input int distance);
        event_pulse(code, start_track, distance);
        repeat (TICK_CLOCKS) @(negedge clk);
    endtask

    // This task samples only after the player's output scaling pipeline has
    // settled, then leaves time for the next PCM read, including delayed DDR.
    task automatic tick_value(input logic signed [15:0] expected);
        @(negedge clk); audio_tick = 1;
        @(negedge clk); audio_tick = 0;
        repeat (4) @(negedge clk);
        if (audio_l !== expected || audio_r !== expected)
            $fatal(1, "PCM check %0d: got L=%0d R=%0d, expected %0d",
                   sample_checks, audio_l, audio_r, expected);
        sample_checks++;
        repeat (TICK_CLOCKS - 5) @(negedge clk);
    endtask

    task automatic unchecked_tick;
        @(negedge clk); audio_tick = 1;
        @(negedge clk); audio_tick = 0;
        repeat (TICK_CLOCKS - 1) @(negedge clk);
    endtask

    task automatic quiet_after_timeout;
        repeat (4) unchecked_tick();
        repeat (8) tick_value(0);
    endtask

    function automatic int seek_start(input int code, input int track);
        int position;
        logic [2:0] clip;
        begin
            position = track > 140 ? 140 : track;
            clip = code == 1 ? DISK2_SOUND_SEEK_0_34 : DISK2_SOUND_SEEK_34_0;
            if (code == 2)
                position = 140 - position;
            seek_start = disk2_sound_offset(clip) + disk2_sound_seek_position_offset(clip, position);
        end
    endfunction

    task automatic boot_seek(input bit delay_reads);
        int cursor, lower, upper, elapsed, next_event, event_number;
        begin
            reset_player();
            delayed_memory = delay_reads;
            lower = disk2_sound_offset(DISK2_SOUND_SEEK_34_0);
            upper = lower + disk2_sound_length(DISK2_SOUND_SEEK_34_0);
            cursor = seek_start(2, 32);
            ready_event(2, 32, 2);
            event_number = 1;
            next_event = (19690 * 48) / 1023;
            for (elapsed = 0; elapsed < 15 * 19690 * 48 / 1023 + SEEK_HOLD_SAMPLES; elapsed++) begin
                if (event_number < 16 && elapsed == next_event) begin
                    event_pulse(2, 32 - event_number * 2, 2);
                    event_number++;
                    next_event = (event_number * 19690 * 48) / 1023;
                end
                tick_value(pcm(cursor));
                cursor++;
                if (cursor == upper)
                    cursor = lower;
            end
            quiet_after_timeout();
            $display("DISK2 SOUND boot cadence, wrap, timeout: delayed=%0d", delay_reads);
        end
    endtask

    task automatic isolated_and_boundary;
        int cursor, i;
        begin
            reset_player();
            ready_event(1, 24, 2);
            cursor = seek_start(1, 24);
            for (i = 0; i < SEEK_HOLD_SAMPLES - 1; i++) begin
                tick_value(pcm(cursor)); cursor++;
            end
            // Refresh on the very audio edge that would expire the seek.
            @(negedge clk);
            sound_event = 1; seek_start_qtrack = 26; seek_distance = 2; audio_tick = 1;
            @(negedge clk); sound_event = 0; audio_tick = 0;
            repeat (4) @(negedge clk);
            if (audio_l !== pcm(cursor) || audio_r !== pcm(cursor))
                $fatal(1, "refresh/timeout coincidence changed the current PCM sample");
            cursor++;
            repeat (TICK_CLOCKS - 5) @(negedge clk);
            for (i = 0; i < SEEK_HOLD_SAMPLES - 1; i++) begin
                tick_value(pcm(cursor)); cursor++;
            end
            quiet_after_timeout();
            $display("DISK2 SOUND isolated seek and refresh/timeout coincidence");
        end
    endtask

    task automatic event_rules;
        int cursor, i;
        begin
            reset_player();
            ready_event(1, 60, 2);
            cursor = seek_start(1, 60);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            ready_event(2, 62, 2);
            cursor = seek_start(2, 62);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            ready_event(2, 60, 0);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            // Recalibration replaces the seek, then ignores further step,
            // recalibration and door triggers while its clip is playing.
            ready_event(3, 0, 0);
            cursor = disk2_sound_offset(DISK2_SOUND_TRACK0_RECAL);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            for (i = 1; i <= 5; i++) begin
                ready_event(i, 32, 2);
                repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            end
            reset_player();
            ready_event(1, 60, 2);
            tick_value(pcm(seek_start(1, 60)));
            ready_event(4, 0, 0);
            cursor = disk2_sound_offset(DISK2_SOUND_DOOR_OPEN);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            ready_event(5, 0, 0);
            cursor = disk2_sound_offset(DISK2_SOUND_DOOR_CLOSE);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            $display("DISK2 SOUND direction, zero distance, recal and door priorities");
        end
    endtask

    task automatic clamp_rules;
        int cursor;
        begin
            reset_player();
            ready_event(1, 255, 255);
            repeat (8) tick_value(0);
            ready_event(2, 0, 255);
            repeat (8) tick_value(0);
            ready_event(1, 40, 0);
            repeat (8) tick_value(0);
            ready_event(2, 255, 255);
            cursor = seek_start(2, 255);
            repeat (20) begin tick_value(pcm(cursor)); cursor++; end
            reset_player();
            ready_event(1, 139, 255);
            cursor = seek_start(1, 139);
            repeat (400) begin
                tick_value(pcm(cursor)); cursor++;
                if (cursor == DISK2_SOUND_SAMPLE_COUNT)
                    cursor = disk2_sound_offset(DISK2_SOUND_SEEK_0_34);
            end
            $display("DISK2 SOUND clamped positions and movement beyond recorded range");
        end
    endtask

    task automatic stale_response(input int replacement, input bit stall_address);
        int timeout, cursor;
        begin
            reset_player();
            delayed_memory = 1;
            force_ar_stall = stall_address;
            hold_response = 1;
            event_pulse(1, 60, 2);
            repeat (20) @(negedge clk);
            if (stall_address) begin
                event_pulse(replacement, 62, 2);
                repeat (20) @(negedge clk);
            end
            force_ar_stall = 0;
            timeout = 0;
            while (!response_pending && timeout < 100) begin
                @(negedge clk); timeout++;
            end
            if (!response_pending)
                $fatal(1, "held-response test did not reach an outstanding read");
            if (!stall_address)
                event_pulse(replacement, 62, 2);
            repeat (20) @(negedge clk);
            hold_response = 0;
            repeat (TICK_CLOCKS * 2) @(negedge clk);
            cursor = replacement == 2 ? seek_start(2, 62) :
                     disk2_sound_offset(DISK2_SOUND_TRACK0_RECAL);
            repeat (40) begin tick_value(pcm(cursor)); cursor++; end
            $display("DISK2 SOUND outstanding read across replacement event=%0d AR stall=%0d", replacement, stall_address);
        end
    endtask

    task automatic zero_refresh_and_motor_stop;
        int cursor, i;
        begin
            reset_player();
            ready_event(1, 40, 2);
            cursor = seek_start(1, 40);
            for (i = 0; i < SEEK_HOLD_SAMPLES; i++) begin
                if (i == SEEK_HOLD_SAMPLES - 10)
                    event_pulse(1, 42, 0);
                if (i == SEEK_HOLD_SAMPLES - 5)
                    event_pulse(1, 255, 2);
                tick_value(pcm(cursor)); cursor++;
            end
            quiet_after_timeout();
            ready_event(1, 40, 2);
            tick_value(pcm(seek_start(1, 40)));
            @(negedge clk); drive_spinning = 0;
            repeat (TICK_CLOCKS) @(negedge clk);
            repeat (8) tick_value(0);
            ready_event(1, 40, 2);
            repeat (8) tick_value(0);
            ready_event(4, 0, 0);
            cursor = disk2_sound_offset(DISK2_SOUND_DOOR_OPEN);
            repeat (10) begin tick_value(pcm(cursor)); cursor++; end
            $display("DISK2 SOUND zero/clamped event does not refresh and motor stop ends seek");
        end
    endtask

    task automatic disable_rules;
        int mode;
        begin
            for (mode = 0; mode < 4; mode++) begin
                reset_player();
                ready_event(1, 30, 2);
                tick_value(pcm(seek_start(1, 30)));
                @(negedge clk);
                case (mode)
                    0: rstn = 0;
                    1: enable = 0;
                    2: volume = 0;
                    3: sample_base_addr = 0;
                endcase
                repeat (8) @(negedge clk);
                if (audio_l !== 0 || audio_r !== 0 || sample_read.arvalid)
                    $fatal(1, "mute/reset/disable mode %0d did not silence and stop reads", mode);
                @(negedge clk);
                rstn = 1; enable = 1; volume = 10; sample_base_addr = SAMPLE_BASE;
                repeat (8) @(negedge clk);
                repeat (8) tick_value(0);
            end
            $display("DISK2 SOUND reset, disable, mute and missing sample base");
        end
    endtask

    initial begin
        boot_seek(0);
        boot_seek(1);
        isolated_and_boundary();
        event_rules();
        clamp_rules();
        stale_response(2, 0);
        stale_response(3, 0);
        stale_response(2, 1);
        stale_response(3, 1);
        zero_refresh_and_motor_stop();
        disable_rules();
        $display("DISK2 SOUND PASS: %0d exact stereo samples, %0d DDR reads", sample_checks, request_count);
        $finish;
    end

    initial begin
        #200000000;
        $fatal(1, "Disk II sound regression timed out");
    end
endmodule
