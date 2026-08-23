`timescale 1ns / 1ps

module tb_apple_virtual_bus;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic video_mode_50hz = 1'b0;
    logic res_n_in = 1'b1;
    logic irq_n_in = 1'b1;
    logic nmi_n_in = 1'b1;
    logic rdy_n_in = 1'b1;
    logic dma_n_in = 1'b1;
    logic inh_n_in = 1'b1;

    logic        req_valid = 1'b0;
    logic        req_ready;
    logic [15:0] req_addr = '0;
    logic        req_rw = 1'b1;
    logic [7:0]  req_wdata = '0;
    logic        resp_valid;
    logic [7:0]  resp_rdata;
    logic [7:0]  floating_bus_data = 8'h3C;

    globals::AppleBus_write slot_write = '0;
    globals::AppleBus_read  ab_read;
    logic [7:0]             sampled_data;

    apple_virtual_bus dut (
        .clk(clk),
        .resetn(resetn),
        .video_mode_50hz(video_mode_50hz),
        .res_n_in(res_n_in),
        .irq_n_in(irq_n_in),
        .nmi_n_in(nmi_n_in),
        .rdy_n_in(rdy_n_in),
        .dma_n_in(dma_n_in),
        .inh_n_in(inh_n_in),
        .req_valid(req_valid),
        .req_ready(req_ready),
        .req_addr(req_addr),
        .req_rw(req_rw),
        .req_wdata(req_wdata),
        .resp_valid(resp_valid),
        .resp_rdata(resp_rdata),
        .floating_bus_data(floating_bus_data),
        .ab_write(slot_write),
        .ab_read(ab_read),
        .sampled_data(sampled_data)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $fatal(1, "%s", message);
        end
    endtask

    int phase_order = 0;
    int complete_cycles = 0;
    int strobe_count;
    int fabric_clock_count = 0;
    logic [15:0] held_cycle_addr;
    logic        held_cycle_rw;
    logic        held_cycle_valid = 1'b0;

    // Check the public phase contract without reading internal state.
    always @(posedge clk) begin
        fabric_clock_count++;
        if (resetn) begin
            strobe_count = ab_read.drive_en + ab_read.addr_en +
                           ab_read.sss_en + ab_read.serve_en +
                           ab_read.data_en;
            check(strobe_count <= 1, "virtual bus strobes overlap");
            check(ab_read.cycle_valid, "virtual //e cycle was invalid");

            if (ab_read.drive_en) begin
                check(phase_order == 0, "drive_en phase order");
                check(!ab_read.phi0, "drive_en must occur in PHI1");
                phase_order = 1;
            end
            if (ab_read.addr_en) begin
                check(phase_order == 1, "addr_en phase order");
                check(!ab_read.phi0, "addr_en must occur in PHI1");
                held_cycle_addr = ab_read.addr;
                held_cycle_rw = ab_read.rw;
                held_cycle_valid = 1'b1;
                phase_order = 2;
            end
            if (ab_read.sss_en) begin
                check(phase_order == 2, "sss_en phase order");
                check(!ab_read.phi0, "sss_en must occur in PHI1");
                check(held_cycle_valid &&
                      (ab_read.addr == held_cycle_addr) &&
                      (ab_read.rw == held_cycle_rw),
                      "cycle tuple changed before sss_en");
                phase_order = 3;
            end
            if (ab_read.serve_en) begin
                check(phase_order == 3, "serve_en phase order");
                check(ab_read.phi0, "serve_en must occur in PHI0");
                check(held_cycle_valid &&
                      (ab_read.addr == held_cycle_addr) &&
                      (ab_read.rw == held_cycle_rw),
                      "cycle tuple changed before serve_en");
                phase_order = 4;
            end
            if (ab_read.data_en) begin
                check(phase_order == 4, "data_en phase order");
                check(ab_read.phi0, "data_en must occur in PHI0");
                check(sampled_data == ab_read.data,
                      "sampled data must match the public data phase");
                check(held_cycle_valid &&
                      (ab_read.addr == held_cycle_addr) &&
                      (ab_read.rw == held_cycle_rw),
                      "cycle tuple changed before data_en");
                held_cycle_valid = 1'b0;
                phase_order = 0;
                complete_cycles++;
            end
        end
    end

    task automatic check_cycle_cadence(input int expected_clocks);
        int first_sss_clock;
        int second_sss_clock;
        @(posedge clk iff ab_read.sss_en);
        first_sss_clock = fabric_clock_count;
        @(posedge clk iff ab_read.sss_en);
        second_sss_clock = fabric_clock_count;
        check((second_sss_clock - first_sss_clock) == expected_clocks,
              $sformatf("virtual bus cadence was %0d clocks, expected %0d",
                        second_sss_clock - first_sss_clock,
                        expected_clocks));
    endtask

    task automatic submit_request(
        input logic [15:0] addr,
        input logic        rw,
        input logic [7:0]  wdata
    );
        req_addr   <= addr;
        req_rw     <= rw;
        req_wdata  <= wdata;
        req_valid  <= 1'b1;
        do begin
            @(posedge clk);
        end while (!req_ready);
        req_valid <= 1'b0;
    endtask

    initial begin
        repeat (5) @(posedge clk);
        resetn <= 1'b1;

        wait (complete_cycles >= 2);
        check(ab_read.addr == 16'hFFFF && ab_read.rw,
              "idle cycles must park on a $FFFF read");

        // A registered slot response may begin after serve_en and must be
        // sampled at data_en.
        submit_request(16'hC0A0, 1'b1, 8'h00);
        @(posedge clk iff ab_read.addr_en);
        check(ab_read.addr_early == 16'hC0A0 && ab_read.rw_early,
              "CPU read early address phase");
        @(posedge clk iff ab_read.sss_en);
        check(ab_read.addr == 16'hC0A0, "CPU read sss phase");
        @(posedge clk iff ab_read.serve_en);
        check(ab_read.addr == 16'hC0A0 && ab_read.rw,
              "CPU read serve phase");
        check(ab_read.data == 8'h3C,
              "pre-data floating bus was not live at serve_en");
        slot_write.wr_data    <= 8'hA5;
        slot_write.wr_data_en <= 1'b1;
        @(posedge clk iff ab_read.data_en);
        #1;
        check(resp_valid && resp_rdata == 8'hA5,
              "slot read response did not complete the CPU request");
        slot_write.wr_data_en <= 1'b0;

        // The bus captures the resolved byte on the clock before data_en.
        // A card may drive until that edge, but a later source change must not
        // alter the public data phase or the CPU response.
        submit_request(16'hC0A4, 1'b1, 8'h00);
        @(posedge clk iff ab_read.serve_en);
        slot_write.wr_data    <= 8'hD1;
        slot_write.wr_data_en <= 1'b1;
        @(negedge clk iff dut.phase_q == 7'd123);
        @(posedge clk);
        slot_write.wr_data    <= 8'hE2;
        slot_write.wr_data_en <= 1'b0;
        #1;
        check(ab_read.data_en && ab_read.data == 8'hD1,
              "resolved data was not held across data_en");
        @(posedge clk iff ab_read.data_en);
        #1;
        check(resp_valid && resp_rdata == 8'hD1,
              "CPU response did not use the held data byte");

        // Writes place the CPU byte on ab_read.data for card side effects.
        submit_request(16'hC0A1, 1'b0, 8'h5A);
        @(posedge clk iff ab_read.serve_en);
        check(ab_read.data == 8'h5A,
              "CPU write byte was not live before data_en");
        @(posedge clk iff ab_read.data_en);
        check(ab_read.addr == 16'hC0A1 && !ab_read.rw &&
              ab_read.data == 8'h5A,
              "CPU write was not visible at data_en");
        #1;
        check(resp_valid, "CPU write did not complete");

        // An unserved read uses the scanner/floating-bus byte.
        submit_request(16'hC050, 1'b1, 8'h00);
        @(posedge clk iff ab_read.data_en);
        #1;
        check(resp_valid && resp_rdata == 8'h3C,
              "floating-bus fallback was not returned");

        // Each slot assertion pulls its matching active-low line down.
        slot_write.assert_inh <= 1'b1;
        slot_write.assert_res <= 1'b1;
        slot_write.assert_irq <= 1'b1;
        slot_write.assert_rdy <= 1'b1;
        slot_write.assert_nmi <= 1'b1;
        slot_write.assert_dma <= 1'b1;
        #1;
        check(!ab_read.inh && !ab_read.res && !ab_read.irq &&
              !ab_read.rdy && !ab_read.nmi && !ab_read.dma,
              "open-drain slot-line fold");
        slot_write <= '0;

        // A posted-write style master may not load its registered address
        // until two clocks after drive_en. The resolve window must still catch
        // it before addr_en and hold it through data_en.
        @(posedge clk iff ab_read.drive_en);
        slot_write.assert_dma <= 1'b1;
        repeat (2) @(posedge clk);
        slot_write.wr_addr       <= 16'h2345;
        slot_write.wr_rw         <= 1'b0;
        slot_write.wr_addr_rw_en <= 1'b1;
        slot_write.wr_data       <= 8'h7B;
        slot_write.wr_dma_data_en <= 1'b1;
        @(posedge clk iff ab_read.addr_en);
        check(ab_read.addr == 16'h2345 && !ab_read.rw,
              "late registered master missed the resolve window");
        @(posedge clk iff ab_read.data_en);
        check(ab_read.addr == 16'h2345 && !ab_read.rw &&
              ab_read.data == 8'h7B,
              "late registered master tuple was not held through data_en");
        slot_write <= '0;

        // RDY repeats the current CPU cycle and withholds resp_valid.
        submit_request(16'hC0A2, 1'b1, 8'h00);
        @(posedge clk iff ab_read.serve_en);
        slot_write.wr_data    <= 8'h66;
        slot_write.wr_data_en <= 1'b1;
        slot_write.assert_rdy <= 1'b1;
        @(posedge clk iff ab_read.data_en);
        #1;
        check(!resp_valid, "RDY-held request completed early");
        @(posedge clk iff ab_read.drive_en);
        check(!req_ready, "RDY-held request freed its queue entry");
        @(posedge clk iff ab_read.serve_en);
        check(ab_read.addr == 16'hC0A2, "RDY did not repeat the address");
        slot_write.assert_rdy <= 1'b0;
        @(posedge clk iff ab_read.data_en);
        #1;
        check(resp_valid && resp_rdata == 8'h66,
              "RDY-released request did not complete");
        slot_write.wr_data_en <= 1'b0;

        // A slot DMA master blocks a waiting CPU request and owns address,
        // R/W, and write data until it releases the virtual bus.
        req_addr   <= 16'hC0A3;
        req_rw     <= 1'b1;
        req_valid  <= 1'b1;
        slot_write.assert_dma <= 1'b1;
        @(posedge clk iff ab_read.drive_en);
        check(!req_ready && !ab_read.dma,
              "DMA assertion did not hold the CPU request");
        slot_write.wr_addr       <= 16'h2000;
        slot_write.wr_rw         <= 1'b0;
        slot_write.wr_addr_rw_en <= 1'b1;
        slot_write.wr_data       <= 8'hD6;
        slot_write.wr_dma_data_en <= 1'b1;
        @(posedge clk iff ab_read.addr_en);
        check(ab_read.addr_early == 16'h2000 && !ab_read.rw_early,
              "DMA early address phase");
        @(posedge clk iff ab_read.data_en);
        check(ab_read.addr == 16'h2000 && !ab_read.rw &&
              ab_read.data == 8'hD6,
              "DMA owner did not drive the virtual bus");
        #1;
        check(!resp_valid, "DMA cycle completed the waiting CPU request");
        slot_write <= '0;

        do begin
            @(posedge clk);
        end while (!req_ready);
        req_valid <= 1'b0;
        @(posedge clk iff ab_read.data_en);
        #1;
        check(resp_valid && resp_rdata == 8'h3C,
              "CPU request did not resume after DMA release");

        video_mode_50hz <= 1'b0;
        check_cycle_cadence(130);
        video_mode_50hz <= 1'b1;
        check_cycle_cadence(131);

        $display("APPLE VIRTUAL BUS PASS");
        $finish;
    end

    initial begin
        #100000;
        $fatal(1, "APPLE VIRTUAL BUS TIMEOUT");
    end

endmodule
