`timescale 1ns / 1ps

// Pin-level Disk II response regression.
//
// This bench carries physical slot-I/O cycles through the same path used in
// hardware:
//
//   Apple pins -> apple_bus_wrapper -> disk2_card
//              -> 12-client apple_bus_write_arbiter -> apple_bus_wrapper
//              -> Apple data pins
//
// It checks the registered physical response tuple in apple_bus_wrapper and
// the two host-specific release rules. A //e response must release at the raw
// PHI0 fall. A II/II+ response must retain its saved byte across that fall,
// then release when the synchronized fall reaches the wrapper.
module tb_disk2_physical_bus;
    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75ns clk = ~clk;       // 133.333 MHz fabric clock

    logic phi0 = 1'b0;
    always #490ns phi0 = ~phi0;      // about 1.02 MHz Apple clock

    logic rstn = 1'b0;
    logic host_is_iiplus = 1'b0;

    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
    wire        apple_inh_pin;
    wire        apple_res_pin;
    wire        apple_irq_pin;
    wire        apple_rdy_pin;
    wire        apple_dma_pin;
    wire        apple_nmi_pin;

    logic        cpu_addr_oe = 1'b0;
    logic [15:0] cpu_addr = 16'hFFFF;
    logic        cpu_rw = 1'b1;
    logic        cpu_data_oe = 1'b0;
    logic [7:0]  cpu_data = 8'hFF;

    assign apple_addr_pin = cpu_addr_oe ? cpu_addr : 16'hzzzz;
    assign apple_rw_pin   = cpu_addr_oe ? cpu_rw : 1'bz;
    assign apple_data_pin = cpu_data_oe ? cpu_data : 8'hzz;

    // Released motherboard lines are pulled high. The card and CPU drivers
    // above are strong, so these weak assignments only make high-Z periods
    // deterministic in simulation.
    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin   = 1'b1;
    assign (weak0, weak1) apple_inh_pin  = 1'b1;
    assign (weak0, weak1) apple_res_pin  = 1'b1;
    assign (weak0, weak1) apple_irq_pin  = 1'b1;
    assign (weak0, weak1) apple_rdy_pin  = 1'b1;
    assign (weak0, weak1) apple_dma_pin  = 1'b1;
    assign (weak0, weak1) apple_nmi_pin  = 1'b1;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write disk_write;
    globals::AppleBus_write [11:0] client_writes;
    globals::AppleBus_write ab_write;
    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    logic [20:0] mc_line_addr;
    logic        mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0]  mc_wstrb;
    logic        mc_valid;
    logic        mc_ready = 1'b1;
    logic [63:0] mc_rdata = 64'h0;
    logic        mc_rvalid = 1'b0;

    logic tini_oe_pin;
    logic tini_addr_dir_pin;
    logic tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk),
        .rstn(rstn),
        .res_filtered_out(),
        .dbg_lost_cycle_count(),
        .dbg_bus_quality(),
        .dbg_tap_mismatch(),
        .dbg_strobe_anom(),
        .dbg_tap_last(),
        .dbg_ghost_write(),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1),
        .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0),
        .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(1'b1),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read),
        .ab_write(ab_write)
    );

    // Match apple_top's production arbiter width and Disk II client index.
    // Client 0 has highest priority; Disk II is client 3 in apple_top.
    always_comb begin
        client_writes = '{default: '0};
        client_writes[3] = disk_write;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(12),
        .FAST_DATA_CLIENT(2),
        .FAST_ADDR_CLIENT(11)
    ) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes(client_writes),
        .ab_write(ab_write)
    );

    disk2_card card_i (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .rom_serve_en(1'b0),
        .sss(sss),
        .slot_assign(3'd6),
        .as_common(as_common),
        .as_client(axi),
        .mc_line_addr(mc_line_addr),
        .mc_rw(mc_rw),
        .mc_wdata(mc_wdata),
        .mc_wstrb(mc_wstrb),
        .mc_valid(mc_valid),
        .mc_ready(mc_ready),
        .mc_rdata(mc_rdata),
        .mc_rvalid(mc_rvalid),
        .ab_write(disk_write),
        .vtw_active(1'b0),
        .vtw_req_valid(1'b0),
        .vtw_req_addr(4'h0),
        .vtw_req_ready(),
        .vtw_resp_valid(),
        .vtw_resp_rdata(),
        .vtw_cycle_tick(1'b0),
        .vtw_native_cycle_active(1'b0),
        .vtw_time_ready(),
        .vtw_write_timing_active(),
        .sound_spinning(),
        .sound_qtrack(),
        .sound_event(),
        .sound_seek_start_qtrack(),
        .sound_seek_distance()
    );

    task automatic check(input bit condition, input string message);
        if (!condition)
            $fatal(1, "FAIL: %s", message);
    endtask

    // Check the new wrapper boundary directly. The values sampled from the
    // arbiter at a fabric edge must appear as one coherent tuple after that
    // edge. This also catches a data byte and enable becoming misaligned.
    logic stage_monitor_enable = 1'b0;
    integer fabric_edge_count = 0;
    integer last_serve_edge = -1;
    integer last_sync_fall_edge = -1;
    realtime last_fabric_edge_at = 0.0;
    realtime last_sync_fall_at = 0.0;
    logic sampled_data_en;
    logic [7:0] sampled_data;
    logic sampled_addr_rw_en;
    logic sampled_rw;
    logic sampled_inh;
    logic sampled_serve;
    logic sampled_sync_fall;
    always @(posedge clk) begin
        fabric_edge_count = fabric_edge_count + 1;
        last_fabric_edge_at = $realtime;
        sampled_data_en = ab_write.wr_data_en;
        sampled_data = ab_write.wr_data;
        sampled_addr_rw_en = ab_write.wr_addr_rw_en;
        sampled_rw = ab_write.wr_rw;
        sampled_inh = ab_write.assert_inh;
        sampled_serve = ab_read.serve_en &&
                        (ab_read.addr[15:4] == 12'hC0E);
        sampled_sync_fall = wrapper_i.phi0_fall;
        if (sampled_serve)
            last_serve_edge = fabric_edge_count;
        if (sampled_sync_fall) begin
            last_sync_fall_edge = fabric_edge_count;
            last_sync_fall_at = $realtime;
        end
        #1ps;
        if (stage_monitor_enable) begin
            check(wrapper_i.physical_data_en_q === sampled_data_en,
                  "physical data enable did not follow the arbiter by one register edge");
            check(wrapper_i.physical_data_q === sampled_data,
                  "physical data byte did not stay aligned with its enable");
            check(wrapper_i.physical_addr_rw_en_q === sampled_addr_rw_en,
                  "physical address ownership tag was not staged with data");
            check(wrapper_i.physical_rw_q === sampled_rw,
                  "physical R/W tag was not staged with data");
            check(wrapper_i.physical_inh_dependent_q === sampled_inh,
                  "physical INH-dependency tag was not staged with data");
        end
    end

    bit allow_data_drive = 1'b0;
    int data_drive_count = 0;
    always @(posedge tini_data_dir_pin) begin
        if (rstn) begin
            check(allow_data_drive,
                  "Disk II drove the data bus outside an expected read response");
            data_drive_count++;
        end
    end

    task automatic cpu_write(input logic [15:0] addr,
                             input logic [7:0] value);
        @(negedge phi0);
        #50ns;
        cpu_addr = addr;
        cpu_rw = 1'b0;
        cpu_data = value;
        cpu_addr_oe = 1'b1;
        cpu_data_oe = 1'b1;
        @(posedge phi0);
        #300ns;
        check(!tini_data_dir_pin,
              $sformatf("Disk II drove during write to $%04X", addr));
        @(negedge phi0);
        #80ns;
        check(!tini_data_dir_pin,
              $sformatf("Disk II leaked a response after write to $%04X", addr));
        cpu_addr_oe = 1'b0;
        cpu_data_oe = 1'b0;
        cpu_rw = 1'b1;
    endtask

    // Put a known, non-pullup value in the Disk II latch through the physical
    // Q6/Q7 write sequence. This proves that the later byte came through the
    // controller, arbiter, response register, and pins.
    task automatic load_disk_latch(input logic [7:0] value);
        cpu_write(16'hC0ED, 8'h00);  // Q6 high
        cpu_write(16'hC0EF, value);  // Q7 high + load data latch
        repeat (3) @(posedge clk);
        #1ps;
        check(card_i.disk_latch_q === value,
              $sformatf("physical write loaded %02X, latch contains %02X",
                        value, card_i.disk_latch_q));
    endtask

    realtime last_dir_fall_at = 0.0;
    always @(negedge tini_data_dir_pin)
        last_dir_fall_at = $realtime;

    task automatic cpu_read_response(input logic [15:0] addr,
                                     input logic [7:0] expected,
                                     input bit iiplus_mode);
        realtime rise_at;
        realtime fall_at;
        realtime drive_at;
        realtime release_at;
        integer drive_edge;
        integer serve_to_drive_edges;

        host_is_iiplus = iiplus_mode;
        @(negedge phi0);
        #50ns;
        cpu_addr = addr;
        cpu_rw = 1'b1;
        cpu_addr_oe = 1'b1;
        cpu_data_oe = 1'b0;
        allow_data_drive = 1'b1;
        last_dir_fall_at = 0.0;

        @(posedge phi0);
        rise_at = $realtime;
        fork : wait_for_drive
            begin
                @(posedge tini_data_dir_pin);
            end
            begin
                #450ns;
                $fatal(1, "FAIL: no physical response for read $%04X", addr);
            end
        join_any
        disable wait_for_drive;
        drive_at = $realtime;
        drive_edge = fabric_edge_count;
        serve_to_drive_edges = drive_edge - last_serve_edge;

        #1ps;
        check(apple_data_pin === expected,
              $sformatf("read $%04X drove %02X, expected %02X",
                        addr, apple_data_pin, expected));
        check(drive_at == last_fabric_edge_at,
              "physical response did not start on a fabric register edge");
        check(last_serve_edge >= 0,
              "physical response started without a Disk II serve event");
        check(serve_to_drive_edges == 16,
              $sformatf("physical response took %0d fabric edges from serve to drive, expected 16",
                        serve_to_drive_edges));

        @(negedge phi0);
        fall_at = $realtime;
        #1ps;
        if (!iiplus_mode) begin
            check(!tini_data_dir_pin,
                  "//e Disk II response did not release at raw PHI0 fall");
            release_at = last_dir_fall_at;
            check(release_at == fall_at,
                  $sformatf("//e release lagged raw PHI0 by %0.3f ns",
                            release_at - fall_at));
        end else begin
            check(tini_data_dir_pin,
                  "II+ Disk II response was not held across raw PHI0 fall");
            check(apple_data_pin === expected,
                  "II+ saved response byte changed at raw PHI0 fall");
            @(negedge tini_data_dir_pin);
            release_at = $realtime;
            #1ps;
            check(release_at == last_sync_fall_at,
                  $sformatf("II+ release %0.3f ns did not match synchronized fall %0.3f ns (edge %0d)",
                            release_at, last_sync_fall_at,
                            last_sync_fall_edge));
            check(last_sync_fall_edge >= 0,
                  "II+ response released without a synchronized PHI0 fall");
        end

        $display("Disk II %s response: start=%0.3f ns, post-fall hold=%0.3f ns, data=%02X",
                 iiplus_mode ? "II+" : "//e",
                 drive_at - rise_at, release_at - fall_at, expected);

        #80ns;
        check(!tini_data_dir_pin,
              "Disk II response remained driven into the next PHI1 phase");
        allow_data_drive = 1'b0;
        cpu_addr_oe = 1'b0;
    endtask

    task automatic cpu_odd_read_no_response(input bit iiplus_mode);
        host_is_iiplus = iiplus_mode;
        @(negedge phi0);
        #50ns;
        cpu_addr = 16'hC0ED;
        cpu_rw = 1'b1;
        cpu_addr_oe = 1'b1;
        cpu_data_oe = 1'b0;
        @(posedge phi0);
        #350ns;
        check(!tini_data_dir_pin,
              "odd Disk II soft-switch read drove the physical data bus");
        @(negedge phi0);
        #80ns;
        check(!tini_data_dir_pin,
              "odd Disk II read left a saved-byte hold active");
        cpu_addr_oe = 1'b0;
    endtask

    initial begin
        sss = '0;
        sss.slot_access = 1'b1;
        as_common = '0;
        axi.awvalid = 1'b0;

        repeat (8) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        stage_monitor_enable = 1'b1;

        // Let the input synchronizers, PHI0 filter, and timing pipes settle.
        repeat (5) @(negedge phi0);
        check(!tini_data_dir_pin, "data direction was active at idle");

        load_disk_latch(8'hA5);
        // $C0EE is both an even read response and Q7-low. Prove that the
        // response survives the state change, then test an odd read with Q7
        // low before entering write mode again.
        cpu_read_response(16'hC0EE, 8'hA5, 1'b0);
        #1ps;
        check(card_i.q7_q === 1'b0,
              "$C0EE even read did not leave Q7 low");
        cpu_odd_read_no_response(1'b0);
        #1ps;
        check(card_i.q7_q === 1'b0,
              "odd read changed the Q7-low state");

        load_disk_latch(8'h3C);
        cpu_read_response(16'hC0EC, 8'h3C, 1'b1);
        #1ps;
        check(card_i.q7_q === 1'b1,
              "$C0EC even read changed the Q7-high state");
        cpu_odd_read_no_response(1'b1);
        #1ps;
        check(card_i.q7_q === 1'b1,
              "odd read changed the Q7-high state");

        check(data_drive_count == 2,
              $sformatf("saw %0d response drive windows, expected 2",
                        data_drive_count));
        check(!tini_data_dir_pin && !tini_addr_dir_pin,
              "test ended with an Apple bus direction pin active");

        $display("DISK2 PHYSICAL BUS PASS");
        $finish;
    end

    initial begin
        #100us;
        $fatal(1, "FAIL: tb_disk2_physical_bus timeout");
    end
endmodule
