`timescale 1ns / 1ps

// A replay of checked native-Phasor writes, not a full Apple/VIA simulation.
// Logical clock ratios come from the CPU/fabric frequencies below. No derived
// fabric clock drives the DUT: raw Q3 passes through the production synchronizer.
module tb_ssi263_calibration;
    logic clk = 0;
    logic rstn = 0;
    logic q3_raw = 0;
    logic xck_ce;
    logic audio_tick = 0;
    logic [1:0] write_strobe = 0;
    logic [2:0] register_id = 0;
    logic [7:0] write_data = 0;
    wire [1:0] d7, irq, done;
    wire signed [15:0] audio [0:1];
    wire [7:0] registers [0:1][0:4];
    longint fabric_hz, cpu_hz, period, origin, capture_start, end_cycle;
    longint cycle_number, q3_phase, audio_phase, tick_number, tick_cycle;
    longint next_cycle, xck_count, sample_count;
    integer trace_file, sample_file, status_file, event_file, config_file;
    integer rc, next_index, next_target, next_register, next_value;
    integer expected_events, event_count;
    logic [1:0] last_d7;
    logic event_now, tick_now;

    always #5 clk = ~clk;

    ssi263_xck_ce clock_enable (.clk(clk), .rstn(rstn), .q3_raw(q3_raw), .xck_ce(xck_ce));
    for (genvar chip = 0; chip < 2; chip++) begin : sockets
        ssi263_voice #(.SSI263_TYPE(2), .HAS_SC01(1'b0)) voice (
            .clk(clk), .rstn(rstn), .apple_res(1'b1), .card_enabled(1'b1),
            .card_mode(3'd5), .audio_tick(audio_tick), .xck_ce(xck_ce),
            .ssi_write_strobe(write_strobe[chip]), .ssi_reg(register_id),
            .ssi_wdata(write_data), .ssi_d7(d7[chip]),
            .votrax_write_strobe(1'b0), .votrax_wdata(8'b0), .via_pcr(8'b0),
            .via_ifr_set(), .via_ifr_clr(), .audio(audio[chip]),
            .direct_irq(irq[chip]), .dbg_backend_done(done[chip]), .dbg_enable_ints()
        );
        assign registers[chip][0] = voice.bus_wrapper_i.duration_phoneme_q;
        assign registers[chip][1] = voice.bus_wrapper_i.inflection_q;
        assign registers[chip][2] = voice.bus_wrapper_i.rate_inflection_q;
        assign registers[chip][3] = voice.bus_wrapper_i.ctrl_art_amp_q;
        assign registers[chip][4] = voice.bus_wrapper_i.filter_freq_q;
    end

    task automatic read_event;
        begin
            rc = $fscanf(trace_file, "%d %d %d %d %d\n", next_cycle, next_index,
                         next_target, next_register, next_value);
            if (rc != 5) begin
                if (!$feof(trace_file)) $fatal(1, "invalid replay stimulus");
                next_cycle = -1;
            end
        end
    endtask

    initial begin
        config_file = $fopen("config.txt", "r");
        if (!config_file) $fatal(1, "cannot open replay configuration");
        rc = $fscanf(config_file, "%d %d %d %d %d %d %d\n", fabric_hz, cpu_hz,
                     period, origin, capture_start, end_cycle, expected_events);
        if (rc != 7) $fatal(1, "invalid replay configuration");
        $fclose(config_file);
        trace_file = $fopen("stimulus.txt", "r");
        sample_file = $fopen("samples.csv", "w");
        status_file = $fopen("status.csv", "w");
        event_file = $fopen("executed_events.csv", "w");
        if (!trace_file || !sample_file || !status_file || !event_file)
            $fatal(1, "cannot open replay files");
        $fwrite(sample_file, "fabric_cycle,target4,target5\n");
        $fwrite(status_file, "fabric_cycle,tick_boundary,d7_target4,d7_target5,irq_target4,irq_target5\n");
        $fwrite(event_file, "index,fabric_cycle,target,register,value,d7_target4,d7_target5\n");
        q3_phase = 0;
        audio_phase = 0;
        tick_number = 0;
        tick_cycle = origin;
        xck_count = 0;
        sample_count = 0;
        event_count = 0;
        last_d7 = 0;
        read_event();
        repeat (16) @(negedge clk);
        rstn = 1;
        for (cycle_number = 0; cycle_number < end_cycle; cycle_number++) begin
            // Two raw rising edges per CPU cycle. Each SSI core applies DIV2.
            q3_phase = q3_phase + 4 * cpu_hz;
            if (q3_phase >= fabric_hz) begin
                q3_phase = q3_phase - fabric_hz;
                q3_raw = !q3_raw;
            end
            audio_phase = audio_phase + 48000;
            audio_tick = (audio_phase >= fabric_hz);
            if (audio_tick) audio_phase = audio_phase - fabric_hz;
            write_strobe = 0;
            event_now = next_cycle == cycle_number;
            if (next_cycle >= 0 && next_cycle < cycle_number)
                $fatal(1, "missed trace event");
            if (event_now) begin
                register_id = next_register[2:0];
                write_data = next_value[7:0];
                case (next_target)
                    4: write_strobe = 2'b01;
                    5: write_strobe = 2'b10;
                    // AY events retain their time/order but produce no audio here.
                    0, 1, 2, 3: write_strobe = 0;
                    default: $fatal(1, "invalid stream target");
                endcase
            end
            tick_now = (cycle_number == tick_cycle);
            @(posedge clk);
            if (xck_ce) xck_count++;
            #1;
            if ($isunknown({d7, irq, audio[0], audio[1]}))
                $fatal(1, "unknown SSI output");
            if (audio_tick && cycle_number >= capture_start) begin
                $fwrite(sample_file, "%0d,%0d,%0d\n", cycle_number, audio[0], audio[1]);
                sample_count++;
            end
            if (tick_now || d7 != last_d7) begin
                $fwrite(status_file, "%0d,%0d,%0d,%0d,%0d,%0d\n", cycle_number,
                        tick_now ? tick_number : -1, d7[0], d7[1], irq[0], irq[1]);
                last_d7 = d7;
            end
            if (tick_now) begin
                tick_number++;
                tick_cycle = origin + (tick_number * period * fabric_hz + cpu_hz - 1) / cpu_hz;
            end
            if (event_now) begin
                if (next_target >= 4 &&
                    registers[next_target - 4][next_register < 4 ? next_register : 4] !== next_value[7:0])
                    $fatal(1, "SSI bus wrapper did not accept trace write %0d", next_index);
                $fwrite(event_file, "%0d,%0d,%0d,%0d,%0d,%0d,%0d\n", next_index,
                        cycle_number, next_target, next_register, next_value, d7[0], d7[1]);
                event_count++;
                read_event();
            end
            @(negedge clk);
        end
        if (next_cycle != -1 || event_count != expected_events)
            $fatal(1, "replay event count mismatch");
        if (xck_count == 0 || sample_count == 0)
            $fatal(1, "clock or capture did not run");
        // Synchronizer latency can leave at most one raw edge in flight.
        if (xck_count < (end_cycle * 2 * cpu_hz / fabric_hz) - 1 ||
            xck_count > (end_cycle * 2 * cpu_hz / fabric_hz) + 1)
            $fatal(1, "Q3 edge count disagrees with CPU clock");
        $fclose(trace_file);
        $fclose(sample_file);
        $fclose(status_file);
        $fclose(event_file);
        $display("CALIBRATION_REPLAY_PASS events=%0d samples=%0d raw_xck_edges=%0d cycles=%0d",
                 event_count, sample_count, xck_count, cycle_number);
        $finish;
    end
endmodule
