`timescale 1ns / 1ps

// Run real consumers side by side: the original live/held bus view and the
// captured-byte view. Exercise both writes and the boot card's read snoop.
module tb_slot2_data_phase;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic resetn = 1'b0;
    logic onee_selected = 1'b1;
    logic bus_enabled = 1'b1;
    logic virtual_res_n = 1'b1;
    logic pal = 1'b0;
    logic vblank = 1'b0;
    logic req_valid = 1'b0;
    logic req_ready;
    logic [15:0] req_addr = 16'hFFFF;
    logic req_rw = 1'b1;
    logic [7:0] req_wdata = 8'd0;
    logic resp_valid;
    logic [7:0] resp_rdata;
    logic [7:0] floating_data = 8'h3C;
    logic [7:0] virtual_data_phase_data;
    logic [7:0] open_apple_data = 8'h80;
    logic late_drive = 1'b0;
    logic [7:0] late_data = 8'd0;
    logic boot_ps_write = 1'b0;
    globals::AppleBus_read virtual_bus;
    globals::AppleBus_read physical_bus;
    globals::AppleBus_read selected_bus;
    globals::AppleBus_read captured_bus;
    globals::AppleBus_read mouse_bus [0:1];
    globals::AppleBus_read boot_bus [0:1];
    globals::AppleBus_write resolved_write;
    globals::AppleBus_write mouse_write [0:1];
    globals::AppleBus_write boot_write [0:1];
    globals::AxiSimple_common as_common;
    globals::SoftSwitchState sss;
    AxiSimple_if mouse_axi [2] ();
    AxiSimple_if boot_axi [2] ();

    function automatic globals::AppleBus_read gate_bus(
        input globals::AppleBus_read value, input logic enabled);
        globals::AppleBus_read result;
        result = value;
        if (!enabled) begin
            result.addr_en = 1'b0;
            result.sss_en = 1'b0;
            result.serve_en = 1'b0;
            result.data_en = 1'b0;
        end
        return result;
    endfunction

    always_comb begin
        selected_bus = onee_selected ? virtual_bus : physical_bus;
        captured_bus = selected_bus;
        captured_bus.data = onee_selected ? virtual_data_phase_data : physical_bus.data;
        mouse_bus[0] = gate_bus(selected_bus, bus_enabled);
        mouse_bus[1] = gate_bus(captured_bus, bus_enabled);
        // Production boot-menu policy disables all private-bus strobes.
        // Only its data byte changes: it always uses the physical input.
        boot_bus[0] = gate_bus(selected_bus, bus_enabled && !onee_selected);
        boot_bus[1] = boot_bus[0];
        boot_bus[1].data = physical_bus.data;
        resolved_write = '0;
        resolved_write.assert_irq = mouse_write[0].assert_irq;
        if (mouse_write[0].wr_data_en) begin
            resolved_write.wr_data_en = 1'b1;
            resolved_write.wr_data = mouse_write[0].wr_data;
        end
        if (boot_write[0].wr_data_en) begin
            resolved_write.wr_data_en = 1'b1;
            resolved_write.wr_data = boot_write[0].wr_data;
        end
        if (virtual_bus.addr == 16'hC061 && virtual_bus.rw) begin
            resolved_write.wr_data_en = 1'b1;
            resolved_write.wr_data = open_apple_data;
        end
        // Change live data at early phases, far from the capture edge. A
        // consumer accidentally using serve-time bytes gets different data.
        if (virtual_bus.sss_en || virtual_bus.serve_en) begin
            resolved_write.wr_data_en = 1'b1;
            resolved_write.wr_data = floating_data ^ 8'hFF;
        end
        if (late_drive) begin
            resolved_write.wr_data_en = 1'b1;
            resolved_write.wr_data = late_data;
        end
    end

    apple_virtual_bus virtual_bus_i (
        .clk(clk), .resetn(resetn), .video_mode_50hz(pal),
        .res_n_in(virtual_res_n), .irq_n_in(1'b1), .nmi_n_in(1'b1),
        .rdy_n_in(1'b1), .dma_n_in(1'b1), .inh_n_in(1'b1),
        .req_valid(req_valid), .req_ready(req_ready),
        .req_addr(req_addr), .req_rw(req_rw), .req_wdata(req_wdata),
        .resp_valid(resp_valid), .resp_rdata(resp_rdata),
        .floating_bus_data(floating_data), .ab_write(resolved_write),
        .ab_read(virtual_bus), .data_phase_data(virtual_data_phase_data)
    );

    for (genvar view = 0; view < 2; view++) begin : consumers
        assign mouse_axi[view].awvalid = 1'b0;
        assign boot_axi[view].awvalid = boot_ps_write;
        mouse_card mouse_i (
            .clk(clk), .rstn(resetn && bus_enabled), .vblank_start_pulse(vblank),
            .ab_read(mouse_bus[view]), .sss(sss), .slot_assign(3'd2),
            .as_common(as_common), .as_client(mouse_axi[view]),
            .ab_write(mouse_write[view]), .dbg_mode(),
            .dbg_vbl_pending(), .dbg_irq_pending()
        );
        boot_menu_card boot_i (
            .clk(clk), .rstn(resetn), .ab_read(boot_bus[view]), .sss(sss),
            .disk2_enabled(1'b1), .apple_video_mode_valid(1'b1),
            .apple_video_mode_50hz(pal), .as_common(as_common),
            .as_client(boot_axi[view]), .ab_write(boot_write[view]),
            .smartport_active(), .disk2_active(), .boot_target_disk2(),
            .configured_boot_target_disk2(), .boot_slot(), .boot_slot_valid(),
            .apple_vblank_start_pulse(), .machine_id(), .machine_id_fault(),
            .iigs_external_slot_mask(), .iigs_external_slot_mask_valid(),
            .iigs_policy_fault(), .aux_probe_pulse(), .aux_status(),
            .aux_status_clear(1'b0)
        );
    end

    int checks = 0;
    int data_edges = 0;
    int outside_differences = 0;
    int physical_edges = 0;
    int failures = 0;
    task automatic check(input bit condition, input string message);
        checks++;
        if (!condition) begin
            failures++;
            $display("FAIL: %s", message);
            if (failures >= 8) $fatal(1, "Too many data-phase failures");
        end
    endtask

    always @(negedge clk) floating_data <= floating_data + 8'h17;

    globals::AppleBus_read old_fields;
    globals::AppleBus_read new_fields;
    always @(posedge clk) begin
        if (resetn) begin
            old_fields = selected_bus;
            new_fields = captured_bus;
            old_fields.data = 8'd0;
            new_fields.data = 8'd0;
            check(old_fields === new_fields, "captured view changed a non-data field");
            if (selected_bus.data_en) begin
                data_edges++;
                check(selected_bus.data === captured_bus.data,
                      "captured byte differed at a consumer's data_en edge");
            end else if (onee_selected && selected_bus.data !== captured_bus.data) begin
                outside_differences++;
            end
            if (!onee_selected) begin
                physical_edges++;
                check(captured_bus === physical_bus,
                      "physical tuple or live data changed");
            end
            #1;
            check(mouse_write[0] === mouse_write[1], "Mouse response/IRQ diverged");
            check(boot_write[0] === boot_write[1], "boot response/control diverged");
            check(mouse_axi[0].rdata === mouse_axi[1].rdata, "Mouse MMIO diverged");
            check(boot_axi[0].rdata === boot_axi[1].rdata, "boot MMIO diverged");
            check({consumers[0].mouse_i.mouse_x_q, consumers[0].mouse_i.mouse_y_q,
                   consumers[0].mouse_i.mode_q, consumers[0].mouse_i.irq_pending_q,
                   consumers[0].mouse_i.clamp_x_min_q, consumers[0].mouse_i.clamp_x_max_q,
                   consumers[0].mouse_i.clamp_y_min_q, consumers[0].mouse_i.clamp_y_max_q}
                  ===
                  {consumers[1].mouse_i.mouse_x_q, consumers[1].mouse_i.mouse_y_q,
                   consumers[1].mouse_i.mode_q, consumers[1].mouse_i.irq_pending_q,
                   consumers[1].mouse_i.clamp_x_min_q, consumers[1].mouse_i.clamp_x_max_q,
                   consumers[1].mouse_i.clamp_y_min_q, consumers[1].mouse_i.clamp_y_max_q},
                  "Mouse position, command, or interrupt state diverged");
            check({consumers[0].boot_i.last_event_q, consumers[0].boot_i.machine_id_q,
                   consumers[0].boot_i.aux_status, consumers[0].boot_i.window_active_q,
                   consumers[0].boot_i.menu_requested_q, consumers[0].boot_i.oa_window_q,
                   consumers[0].boot_i.boot_eligible_q, consumers[0].boot_i.key_count_q,
                   consumers[0].boot_i.key_rd_ptr_q, consumers[0].boot_i.key_wr_ptr_q}
                  ===
                  {consumers[1].boot_i.last_event_q, consumers[1].boot_i.machine_id_q,
                   consumers[1].boot_i.aux_status, consumers[1].boot_i.window_active_q,
                   consumers[1].boot_i.menu_requested_q, consumers[1].boot_i.oa_window_q,
                   consumers[1].boot_i.boot_eligible_q, consumers[1].boot_i.key_count_q,
                   consumers[1].boot_i.key_rd_ptr_q, consumers[1].boot_i.key_wr_ptr_q},
                  "boot command, Open-Apple, report, or FIFO state diverged");
            for (int index = 0; index < 16; index++)
                check(consumers[0].boot_i.slot_c8_ram[index] ===
                      consumers[1].boot_i.slot_c8_ram[index], "C8 scratch byte diverged");
            for (int index = 0; index < 8; index++)
                check(consumers[0].boot_i.key_fifo_q[index] ===
                      consumers[1].boot_i.key_fifo_q[index], "key FIFO byte diverged");
        end
    end

    task automatic settle;
        repeat (3) @(posedge clk);
        #2;
    endtask

    task automatic fabric_reset;
        @(negedge clk);
        resetn = 1'b0;
        req_valid = 1'b0;
        onee_selected = 1'b1;
        bus_enabled = 1'b1;
        virtual_res_n = 1'b1;
        sss = '0;
        sss.slot_access = 1'b1;
        settle();
        @(negedge clk);
        resetn = 1'b1;
        settle();
        check(virtual_data_phase_data === 8'd0, "reset did not clear captured data");
    endtask

    task automatic cpu_transfer(input logic [15:0] address,
                                input logic read_cycle,
                                input logic [7:0] write_data,
                                output logic [7:0] read_data);
        @(negedge clk);
        onee_selected = 1'b1;
        req_addr = address;
        req_rw = read_cycle;
        req_wdata = write_data;
        req_valid = 1'b1;
        do @(posedge clk); while (!req_ready);
        @(negedge clk);
        req_valid = 1'b0;
        do begin
            @(posedge clk);
            #2;
        end while (!resp_valid);
        read_data = resp_rdata;
    endtask

    logic [7:0] read_result;
    task automatic cpu_write(input logic [15:0] address, input logic [7:0] data);
        logic [7:0] ignored;
        cpu_transfer(address, 1'b0, data, ignored);
        // Mouse clamp application and its IRQ output each have a documented
        // following-clock stage after the write itself has completed.
        settle();
    endtask

    task automatic cpu_expect(input logic [15:0] address, input logic [7:0] expected);
        logic [7:0] value;
        cpu_transfer(address, 1'b1, 8'd0, value);
        check(value === expected,
              $sformatf("private read %04x expected %02x, got %02x", address, expected, value));
    endtask

    task automatic boot_ps(input logic [7:0] address, input logic [31:0] data);
        @(negedge clk);
        as_common.awaddr = address;
        as_common.wdata = data;
        as_common.wstrb = 4'hF;
        boot_ps_write = 1'b1;
        @(negedge clk);
        boot_ps_write = 1'b0;
        settle();
    endtask

    task automatic physical_write(input logic [15:0] address, input logic [7:0] data);
        @(negedge clk);
        onee_selected = 1'b0;
        physical_bus.addr = address;
        physical_bus.addr_early = address;
        physical_bus.rw = 1'b0;
        physical_bus.rw_early = 1'b0;
        physical_bus.data = data ^ 8'hFF;
        physical_bus.serve_en = 1'b1;
        @(negedge clk);
        physical_bus.serve_en = 1'b0;
        physical_bus.data = data;
        physical_bus.data_en = 1'b1;
        @(negedge clk);
        physical_bus.data_en = 1'b0;
        physical_bus.rw = 1'b1;
        physical_bus.data = data + 8'h31;
        settle();
    endtask

    task automatic apple_reset;
        @(negedge clk);
        virtual_res_n = 1'b0;
        settle();
        @(negedge clk);
        virtual_res_n = 1'b1;
        settle();
    endtask

    task automatic physical_expect(input logic [15:0] address,
                                   input logic [7:0] expected);
        logic [7:0] value;
        @(negedge clk);
        onee_selected = 1'b0;
        physical_bus.addr = address;
        physical_bus.addr_early = address;
        physical_bus.rw = 1'b1;
        physical_bus.rw_early = 1'b1;
        physical_bus.data = expected ^ 8'hFF;
        physical_bus.serve_en = 1'b1;
        @(posedge clk);
        #2;
        value = boot_write[1].wr_data_en ? boot_write[1].wr_data : expected;
        check(value === expected, $sformatf("physical boot read %04x", address));
        @(negedge clk);
        physical_bus.serve_en = 1'b0;
        physical_bus.data = value;
        physical_bus.data_en = 1'b1;
        @(negedge clk);
        physical_bus.data_en = 1'b0;
        physical_bus.data = value ^ 8'hFF;
        settle();
    endtask

    initial begin
        physical_bus = '0;
        physical_bus.res = 1'b1;
        physical_bus.rw = 1'b1;
        physical_bus.cycle_valid = 1'b1;
        as_common = '0;
        fabric_reset();

        cpu_write(16'hC0A1, 8'h34);
        cpu_write(16'hC0A2, 8'h12);
        cpu_write(16'hC0A3, 8'h78);
        cpu_write(16'hC0A4, 8'h56);
        cpu_expect(16'hC0A1, 8'h34);
        cpu_expect(16'hC0A2, 8'h12);
        check(consumers[1].mouse_i.mouse_y_q === 16'h5678,
              "Mouse did not consume private Y write bytes");
        cpu_write(16'hC0A7, 8'h00);
        cpu_write(16'hC0A8, 8'h00);
        cpu_write(16'hC0A9, 8'h01);
        cpu_write(16'hC0AA, 8'h00);
        cpu_write(16'hC0AB, 8'h02);
        cpu_write(16'hC0AC, 8'h02);
        check(consumers[1].mouse_i.mouse_x_q === 16'h0200,
              "Mouse clamp command used the wrong data byte");
        cpu_write(16'hC0AC, 8'h01);
        cpu_expect(16'hC0A1, 8'h00);
        cpu_expect(16'hC0A2, 8'h01);
        cpu_write(16'hC0AC, 8'h03);
        check(consumers[1].mouse_i.clamp_x_max_q === 16'h03FF,
              "Mouse default-clamp command failed");
        cpu_write(16'hC0AE, 8'h08);
        @(negedge clk); vblank = 1'b1;
        @(negedge clk); vblank = 1'b0;
        settle();
        check(mouse_write[1].assert_irq, "Mouse VBL IRQ setup failed");
        cpu_write(16'hC0AF, 8'h02);
        check(!mouse_write[1].assert_irq, "Mouse IRQ acknowledgement byte failed");

        // A read response may arrive after serve_en. Capture that late byte,
        // then change the live source again during data_en: both consumer
        // views and the CPU response must retain the captured value.
        @(negedge clk);
        late_drive = 1'b1;
        late_data = 8'h11;
        fork
            begin
                cpu_transfer(16'h4000, 1'b1, 8'd0, read_result);
            end
            begin
                do @(negedge clk);
                while (!(virtual_bus.serve_en && virtual_bus.addr == 16'h4000));
                repeat (5) @(negedge clk);
                late_data = 8'h22;
                do @(negedge clk); while (!virtual_bus.data_en);
                late_data = 8'h33;
                #1;
                check(virtual_data_phase_data === 8'h22 && virtual_bus.data === 8'h22,
                      "data phase lost the late response when live data changed");
            end
        join
        check(read_result === 8'h22, "CPU missed the captured late read response");
        @(negedge clk);
        late_drive = 1'b0;

        // Boot consumers run only on physical cycles, exactly as in the top.
        @(negedge clk);
        onee_selected = 1'b0;
        physical_bus.res = 1'b0;
        settle();
        @(negedge clk);
        physical_bus.res = 1'b1;
        settle();
        physical_expect(16'hC061, 8'h80);
        check(!consumers[1].boot_i.oa_window_q && consumers[1].boot_i.boot_eligible_q,
              "boot card did not consume captured Open-Apple read data");
        physical_expect(16'hC700, 8'hA9);
        @(negedge clk); sss.io_select = 8'h80;
        physical_write(16'hC0F0, 8'h01);
        check(consumers[1].boot_i.window_active_q && consumers[1].boot_i.report_session_q,
              "boot WINDOW_BEGIN command did not open a report session");
        physical_write(16'hC0F0, 8'h26);
        physical_write(16'hC0F0, 8'h31);
        check(consumers[1].boot_i.aux_status === 2'b11,
              "boot aux report did not consume captured byte");
        physical_write(16'hC0F0, 8'h22);
        check(consumers[1].boot_i.machine_id_q === 4'd2,
              "boot machine report did not consume captured byte");
        physical_write(16'hCA0F, 8'h5C);
        physical_expect(16'hCA0F, 8'h5C);
        physical_write(16'hC0F0, 8'h03);
        check(consumers[1].boot_i.menu_requested_q, "boot MENU_REQUEST failed");
        physical_write(16'hC0F1, 8'hC1);
        physical_write(16'hC0F1, 8'hD2);
        check(consumers[1].boot_i.key_count_q === 4'd2 &&
              consumers[1].boot_i.key_fifo_q[0] === 8'hC1 &&
              consumers[1].boot_i.key_fifo_q[1] === 8'hD2,
              "boot key FIFO did not consume captured bytes");
        boot_ps(8'h01, 32'h10);
        check(consumers[1].boot_i.key_count_q === 4'd1,
              "boot key FIFO acknowledgement failed");

        // The virtual clock keeps running while the selected physical bus
        // changes bytes before and during its own data phase.
        physical_write(16'hC0A1, 8'hA7);
        physical_write(16'hC0A2, 8'hB6);
        check(consumers[1].mouse_i.mouse_x_q === 16'hB6A7,
              "physical Mouse data was replaced by virtual captured data");
        physical_write(16'hCA0F, 8'h5C);
        check(consumers[1].boot_i.slot_c8_ram[15] === 8'h5C,
              "physical boot scratch write changed");
        physical_write(16'hC0F1, 8'hE3);
        check(consumers[1].boot_i.key_count_q === 4'd2,
              "physical boot key write changed");
        // Switch directly onto a virtual data phase; the same selector must
        // select both its tuple and its already captured byte.
        do @(negedge clk); while (!virtual_bus.data_en);
        onee_selected = 1'b1;
        settle();
        pal = 1'b1;
        cpu_write(16'hC0A1, 8'h6D);
        cpu_expect(16'hC0A1, 8'h6D);
        @(negedge clk); bus_enabled = 1'b0;
        cpu_write(16'hC0A1, 8'hEE);
        @(negedge clk); bus_enabled = 1'b1;
        settle();
        check(consumers[1].mouse_i.mouse_x_q === 16'd0,
              "disabled Mouse consumed a virtual write");
        cpu_write(16'hC0A1, 8'h42);
        cpu_expect(16'hC0A1, 8'h42);

        // Actual top-level policy: boot-menu strobes are off in ONE//e.
        cpu_write(16'hCA0F, 8'h99);
        check(consumers[1].boot_i.slot_c8_ram[15] === 8'h5C,
              "ONEe boot guard allowed a scratch write");
        physical_write(16'hCA0F, 8'h17);
        check(consumers[1].boot_i.slot_c8_ram[15] === 8'h17,
              "boot guard blocked a selected physical write");

        fabric_reset();
        cpu_expect(16'hC0A1, 8'd0);
        check(data_edges > 40 && outside_differences > 500 && physical_edges > 10,
              "insufficient data-phase, changing-live-byte, or physical coverage");
        if (failures != 0) $fatal(1, "SLOT2 DATA PHASE FAIL: %0d failures", failures);
        $display("SLOT2 DATA PHASE PASS: %0d checks, %0d data edges, %0d differing live edges, %0d physical edges",
                 checks, data_edges, outside_differences, physical_edges);
        $finish;
    end

    initial begin
        #1000000;
        $fatal(1, "SLOT2 DATA PHASE FAIL: timeout");
    end
endmodule
