`timescale 1ns/1ps
// Compare real SmartPort/overlay consumers behind extracted old/new top routing.
// The live bus deliberately differs outside data_en, so this proves consumed
// bytes and state, rather than requiring unused combinational values to match.
module tb_smartport_data_phase;
    logic clk = 0;
    always #3.75 clk = ~clk;
    logic rstn = 0, onee = 1, pal = 0, res_n = 1;
    logic visible = 1, overlay_visible = 1, gs_guard = 0;
    logic req_valid = 0, req_ready, req_rw = 1, resp_valid;
    logic [15:0] req_addr = 16'hffff;
    logic [7:0] req_data = 0, resp_data, captured_data, floating_data = 8'h53;
    logic ps_write = 0, capture_drop = 0;
    globals::AppleBus_read virtual_bus, physical_bus, selected_bus, guarded_bus;
    globals::AppleBus_read routed [0:1];
    globals::AppleBus_write bus_write, card_write [0:1];
    globals::AxiSimple_common common;
    globals::SoftSwitchState sss;
    AxiSimple_if axi [2] ();
    logic irq [0:1], armed [0:1], aux [0:1], devsel [0:1];
    logic [15:0] base [0:1], limit [0:1];
    int checks = 0, data_phases = 0, physical_checks = 0, unused_differences = 0;
    int routed_write_phases = 0, routed_read_phases = 0, selected_write_phases = 0;
    int gated_write_phases = 0, reset_write_phases = 0, reset_requests = 0;
    int gated = 0, virtual_writes = 0, physical_writes = 0;
    int serve_differences = 0, changed_after_capture = 0;

    task automatic check(input bit condition, input string message);
        checks++;
        if (!condition) $fatal(1, "SMARTPORT DATA PHASE FAIL: %s at %t", message, $time);
    endtask

    always_comb begin
        selected_bus = onee ? virtual_bus : physical_bus;
        bus_write = card_write[0];
        // Exercise live serve-time values unlike the captured write byte.
        if (virtual_bus.sss_en || virtual_bus.serve_en) begin
            bus_write.wr_data_en = 1;
            bus_write.wr_data = floating_data ^ 8'hff;
        end
        // Change the resolved live reply after capture, while data_en is high.
        if (virtual_bus.data_en) begin
            bus_write.wr_data_en = 1;
            bus_write.wr_data = captured_data ^ 8'h5a;
        end
    end
    always @(negedge clk) floating_data <= floating_data + 8'h17;

    apple_virtual_bus bus_i (
        .clk(clk), .resetn(rstn), .video_mode_50hz(pal),
        .res_n_in(res_n), .irq_n_in(1'b1), .nmi_n_in(1'b1),
        .rdy_n_in(1'b1), .dma_n_in(1'b1), .inh_n_in(1'b1),
        .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_rw(req_rw), .req_wdata(req_data), .resp_valid(resp_valid),
        .resp_rdata(resp_data), .floating_bus_data(floating_data),
        .ab_write(bus_write), .ab_read(virtual_bus), .data_phase_data(captured_data)
    );
    apple_slot7_devsel_guard guard_i (
        .devsel_required(gs_guard), .ab_read_in(selected_bus),
        .physical_ab_read(physical_bus), .supersprite_write_in('0),
        .smartport_write_in('0), .boot_menu_write_in('0),
        .ab_read_out(guarded_bus), .supersprite_write_out(),
        .smartport_write_out(), .boot_menu_write_out()
    );
    smartport_routing_baseline old_route (
        .ab_read(selected_bus), .physical_ab_read(physical_bus),
        .slot7_devsel_ab_read(guarded_bus), .onee_enable_effective(onee),
        .virtual_data_phase_data(captured_data), .vtw_smartport_visible(visible),
        .slot7_overlay_devsel_visible(overlay_visible), .smartport_ab_read(routed[0])
    );
    smartport_routing_candidate new_route (
        .ab_read(selected_bus), .physical_ab_read(physical_bus),
        .slot7_devsel_ab_read(guarded_bus), .onee_enable_effective(onee),
        .virtual_data_phase_data(captured_data), .vtw_smartport_visible(visible),
        .slot7_overlay_devsel_visible(overlay_visible), .smartport_ab_read(routed[1])
    );
    for (genvar v=0; v<2; v++) begin : consumers
        assign axi[v].awvalid = ps_write;
        smartport_card card (
            .clk(clk), .rstn(rstn), .ab_read(routed[v]), .sss(sss),
            .apple_bus_visible(visible), .overlay_bus_visible(overlay_visible),
            .slot_assign(3'd7), .as_common(common), .as_client(axi[v]),
            .ab_write(card_write[v]), .smartport_irq(irq[v]),
            .overlay_capture_drop(capture_drop), .overlay_canvas_shr_active(1'b0),
            .overlay_devsel_enabled(devsel[v]), .overlay_capture_armed(armed[v]),
            .overlay_capture_bank_aux(aux[v]), .overlay_capture_base(base[v]),
            .overlay_capture_limit(limit[v]), .vtw_valid(1'b0), .vtw_target(3'd0),
            .vtw_addr(11'd0), .vtw_rw(1'b1), .vtw_wdata(8'd0),
            .vtw_sss_snapshot(22'd0), .vtw_ready(), .vtw_resp_valid(), .vtw_resp_rdata()
        );
    end

    globals::AppleBus_read old_fields, new_fields;
    always @(posedge clk) begin
        if (rstn) begin
            old_fields = routed[0]; new_fields = routed[1];
            old_fields.data = 0; new_fields.data = 0;
            check(old_fields === new_fields, "non-data gate or bus field changed");
            if (routed[0].data_en) begin
                data_phases++;
                if (routed[0].rw) routed_read_phases++; else routed_write_phases++;
                check(routed[0].data === routed[1].data, "consumed byte changed");
            end else if (onee && routed[0].data !== routed[1].data)
                unused_differences++;
            if (onee && selected_bus.serve_en && routed[0].data !== routed[1].data)
                serve_differences++;
            if (onee && selected_bus.data_en && bus_write.wr_data !== captured_data)
                changed_after_capture++;
            if (selected_bus.data_en && !routed[0].data_en) gated++;
            if (selected_bus.data_en && !selected_bus.rw) begin
                selected_write_phases++;
                if (!routed[0].data_en) gated_write_phases++;
                if (!selected_bus.res) reset_write_phases++;
            end
            if (!onee) begin
                physical_checks++;
                check(routed[0] === routed[1], "physical-host tuple changed");
            end
            #1;
            check(card_write[0] === card_write[1], "card response/control changed");
            check(axi[0].rdata === axi[1].rdata, "PS register read changed");
            check({irq[0],devsel[0],armed[0],aux[0],base[0],limit[0]} ===
                  {irq[1],devsel[1],armed[1],aux[1],base[1],limit[1]}, "capture/IRQ changed");
            check({consumers[0].card.in_count_q, consumers[0].card.in_wr_q,
                   consumers[0].card.out_count_q, consumers[0].card.ready_q,
                   consumers[0].card.exec_pending_q} ===
                  {consumers[1].card.in_count_q, consumers[1].card.in_wr_q,
                   consumers[1].card.out_count_q, consumers[1].card.ready_q,
                   consumers[1].card.exec_pending_q}, "FIFO/command state changed");
            // Includes the exact CE endpoints from the failing physical path.
            check({consumers[0].card.linear_text_overlay_card_i.arm_mul_product_q,
                   consumers[0].card.linear_text_overlay_card_i.arm_mul_step_q,
                   consumers[0].card.linear_text_overlay_card_i.frame_command_q,
                   consumers[0].card.linear_text_overlay_card_i.index_q,
                   consumers[0].card.linear_text_overlay_card_i.busy_q,
                   consumers[0].card.linear_text_overlay_card_i.capture_drop_seen_q} ===
                  {consumers[1].card.linear_text_overlay_card_i.arm_mul_product_q,
                   consumers[1].card.linear_text_overlay_card_i.arm_mul_step_q,
                   consumers[1].card.linear_text_overlay_card_i.frame_command_q,
                   consumers[1].card.linear_text_overlay_card_i.index_q,
                   consumers[1].card.linear_text_overlay_card_i.busy_q,
                   consumers[1].card.linear_text_overlay_card_i.capture_drop_seen_q},
                  "overlay command/multiply state changed");
        end
    end

    task automatic settle;
        repeat (3) @(negedge clk);
    endtask
    task automatic reset_all;
        @(negedge clk); rstn=0; req_valid=0; ps_write=0;
        visible=1; overlay_visible=1; gs_guard=0; capture_drop=0; res_n=1;
        physical_bus='0; physical_bus.res=1; physical_bus.rw=1;
        physical_bus.cycle_valid=1; sss='0; sss.slot_access=1; sss.io_select[7]=1;
        common='0; settle(); rstn=1; settle();
    endtask
    task automatic write_byte(input logic [15:0] address, input logic [7:0] value);
        if (onee) begin
            @(negedge clk); req_addr=address; req_rw=0; req_data=value; req_valid=1;
            do @(posedge clk); while (!req_ready);
            @(negedge clk); req_valid=0;
            do @(negedge clk); while (!resp_valid);
            virtual_writes++;
        end else begin
            @(negedge clk); physical_bus.addr=address; physical_bus.addr_early=address;
            physical_bus.rw=0; physical_bus.rw_early=0;
            physical_bus.data=value ^ 8'hff; physical_bus.serve_en=1;
            @(negedge clk); physical_bus.serve_en=0; physical_bus.data=value;
            physical_bus.data_en=1;
            @(negedge clk); physical_bus.data_en=0; physical_bus.rw=1;
            physical_bus.data=value+8'h31; physical_writes++;
        end
        settle();
    endtask
    task automatic indexed(input logic [7:0] index, input logic [7:0] value);
        write_byte(16'hc0f0,index); write_byte(16'hc0f1,value);
    endtask
    task automatic ps(input logic [7:0] address, input logic [31:0] value);
        @(negedge clk); common.awaddr=address; common.wdata=value;
        common.wstrb=4'hf; ps_write=1;
        @(negedge clk); ps_write=0; settle();
    endtask
    task automatic check_fifo;
        for (int i=0;i<256;i++) begin
            check(consumers[0].card.in_fifo[i] === consumers[1].card.in_fifo[i],
                  "input FIFO byte sequence changed");
            check(consumers[0].card.out_fifo[i] === consumers[1].card.out_fifo[i],
                  "output FIFO byte sequence changed");
        end
    endtask

    initial begin
        for (int mode=0;mode<3;mode++) begin
            onee=(mode!=2); pal=(mode==1); reset_all();
            // All byte values pass through the live arbiter and real FIFO.
            for (int i=0;i<256;i++) write_byte(16'hcff0,8'(i));
            check(consumers[0].card.in_count_q==256,"FIFO test did not consume all bytes");
            check(consumers[0].card.in_fifo[0]===32'h03020100,"FIFO byte order wrong");
            check(consumers[0].card.in_fifo[63]===32'hfffefdfc,"last FIFO bytes wrong");
            check_fifo(); write_byte(16'hcff1,8'h80); ps(8'h03,32'h13);
            // Exercise the timing-critical ARM decode and its eight-clock result.
            indexed(0,0); indexed(1,8'h60); indexed(3,80); indexed(4,24);
            write_byte(16'hc0f3,1); repeat(12) @(negedge clk);
            check(consumers[0].card.linear_text_overlay_card_i.arm_request_q,
                  "ARM command was not consumed");
            ps(8'h28,3);
            check(armed[0] && base[0]==16'h6000 && limit[0]==16'h6f00,
                  "ARM produced wrong capture interval");
            write_byte(16'hc0f3,2); ps(8'h28,4); ps(8'h28,8);
            capture_drop=1; settle(); capture_drop=0; ps(8'h28,8);
            write_byte(16'hc0f3,3); ps(8'h28,8); write_byte(16'hc0f3,0);
            // All payload bits also exercise indexed register writes/reads.
            for (int i=0;i<256;i++) indexed(8'h0d,8'(i));
            // Suppressed slot ownership must not accept even a valid command.
            visible=0; overlay_visible=0; write_byte(16'hc0f0,8'ha5);
            check(consumers[0].card.linear_text_overlay_card_i.index_q==8'h0d,
                  "hidden slot accepted an index write");
            overlay_visible=1; write_byte(16'hc0f0,8'h0e);
            check(consumers[0].card.linear_text_overlay_card_i.index_q==8'h0e,
                  "overlay-only slot failed valid write");
            for (int j=0;j<41;j++) begin
                common.araddr=8'(j); settle();
            end
            if (!onee) begin
                gs_guard=1; physical_bus.devsel_n=1;
                write_byte(16'hc0f0,8'hbc);
                check(consumers[0].card.linear_text_overlay_card_i.index_q==8'h0e,
                      "GS unselected DEVSEL accepted write");
                physical_bus.devsel_n=0; physical_bus.m2sel=1;
                write_byte(16'hc0f0,8'hcd);
                check(consumers[0].card.linear_text_overlay_card_i.index_q==8'h0e,
                      "GS internal cycle accepted write");
                physical_bus.m2sel=0; write_byte(16'hc0f0,8'h0c);
            end
            // Assert RES# after byte capture, before consumers sample data_en.
            // Reset must win over this otherwise valid FIFO write.
            visible=1; overlay_visible=1; gs_guard=0;
            if (onee) begin
                @(negedge clk); req_addr=16'hcff0; req_rw=0;
                req_data=8'ha7; req_valid=1;
                do @(posedge clk); while (!req_ready);
                @(negedge clk); req_valid=0;
                do @(negedge clk); while (!virtual_bus.data_en);
                res_n=0;
            end else begin
                @(negedge clk); physical_bus.addr=16'hcff0; physical_bus.rw=0;
                physical_bus.data=8'ha7; physical_bus.data_en=1; physical_bus.res=0;
            end
            reset_requests++;
            @(negedge clk);
            if (!onee) begin physical_bus.data_en=0; physical_bus.rw=1; end
            settle(); check(!armed[0] && consumers[0].card.in_count_q==0,
                            "reset lost to coincident data-phase write");
            if (onee) res_n=1; else physical_bus.res=1;
            settle(); check_fifo();
        end
        check(data_phases>1500 && virtual_writes>1000 && physical_writes>700,
              "insufficient active bus coverage");
        check(unused_differences>1000 && physical_checks>1000 && gated>=3,
              "test missed changing live bytes, physical mode, or gating");
        check(serve_differences>1000 && changed_after_capture>1000,
              "test missed live-data changes at serve_en or after capture");
        check(reset_write_phases==3, "test missed reset coincident with data_en");
        check(selected_write_phases==virtual_writes+physical_writes+reset_requests &&
              routed_write_phases+gated_write_phases==selected_write_phases &&
              routed_write_phases+routed_read_phases==data_phases,
              "phase/request accounting does not reconcile");
        $display("SMARTPORT DATA PHASE PASS checks=%0d data_phases=%0d routed_write_phases=%0d routed_read_phases=%0d selected_write_phases=%0d gated_write_phases=%0d virtual_write_requests=%0d physical_write_requests=%0d reset_requests=%0d reset_write_phases=%0d outside_differences=%0d serve_differences=%0d changed_after_capture=%0d gated_phases=%0d",
                 checks,data_phases,routed_write_phases,routed_read_phases,
                 selected_write_phases,gated_write_phases,virtual_writes,physical_writes,
                 reset_requests,reset_write_phases,unused_differences,
                 serve_differences,changed_after_capture,gated);
        $finish;
    end
    initial begin #3000000; $fatal(1,"SMARTPORT DATA PHASE timeout"); end
endmodule
