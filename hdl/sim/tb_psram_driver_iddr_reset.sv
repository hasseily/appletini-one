`timescale 1ns / 1ps

// Compare the PSRAM read path with asynchronous, synchronous, and no IDDR
// reset. The Python runner creates the three RTL variants from the production
// source; this bench does not replace the rest of psram_driver with a model.
module tb_psram_driver_iddr_reset;

    logic clk = 1'b0;
    logic resetn = 1'b0;
    always #5 clk = ~clk;

    logic        valid = 1'b0;
    logic [7:0]  cmd = 8'h00;
    logic [23:0] addr = 24'h000000;
    logic [63:0] wdata = 64'h0000000000000000;
    logic        dcount_wr_en = 1'b0;
    logic [4:0]  dcount_wr = 5'd0;
    logic        dcount_edge = 1'b0;

    logic [3:0] pos_a_drive = 4'hf;
    logic [3:0] pos_b_drive = 4'hf;
    logic [3:0] neg_a_drive = 4'hf;
    logic [3:0] neg_b_drive = 4'hf;
    logic [3:0] psram_a_i = 4'hf;
    logic [3:0] psram_b_i = 4'hf;

    // Hold one value around each rising edge and another around each falling
    // edge. The 0.5 ns offset gives the IDELAY/IDDR model a clean setup time.
    always @(negedge clk) begin
        #0.5;
        psram_a_i = pos_a_drive;
        psram_b_i = pos_b_drive;
    end

    always @(posedge clk) begin
        #0.5;
        psram_a_i = neg_a_drive;
        psram_b_i = neg_b_drive;
    end

    wire        prod_ready;
    wire        prod_rvalid;
    wire [63:0] prod_rdata;
    wire        prod_done;
    wire [3:0]  prod_oe;
    wire [3:0]  prod_a_o;
    wire [3:0]  prod_b_o;
    wire        prod_ce_n;
    wire        prod_clk;

    wire        async_ready;
    wire        async_rvalid;
    wire [63:0] async_rdata;
    wire        async_done;
    wire [3:0]  async_oe;
    wire [3:0]  async_a_o;
    wire [3:0]  async_b_o;
    wire        async_ce_n;
    wire        async_clk;

    wire        sync_ready;
    wire        sync_rvalid;
    wire [63:0] sync_rdata;
    wire        sync_done;
    wire [3:0]  sync_oe;
    wire [3:0]  sync_a_o;
    wire [3:0]  sync_b_o;
    wire        sync_ce_n;
    wire        sync_clk;

    wire        norst_ready;
    wire        norst_rvalid;
    wire [63:0] norst_rdata;
    wire        norst_done;
    wire [3:0]  norst_oe;
    wire [3:0]  norst_a_o;
    wire [3:0]  norst_b_o;
    wire        norst_ce_n;
    wire        norst_clk;

`define PSRAM_DRIVER_INSTANCE(MODULE_NAME, INSTANCE_NAME, PREFIX) \
    MODULE_NAME INSTANCE_NAME ( \
        .clk(clk), \
        .resetn(resetn), \
        .valid(valid), \
        .ready(PREFIX``_ready), \
        .cmd(cmd), \
        .addr(addr), \
        .wdata(wdata), \
        .rvalid(PREFIX``_rvalid), \
        .rdata(PREFIX``_rdata), \
        .done(PREFIX``_done), \
        .dcount_wr_en(dcount_wr_en), \
        .dcount_wr(dcount_wr), \
        .dcount_edge(dcount_edge), \
        .psram_oe(PREFIX``_oe), \
        .psram_a_i(psram_a_i), \
        .psram_a_o(PREFIX``_a_o), \
        .psram_b_i(psram_b_i), \
        .psram_b_o(PREFIX``_b_o), \
        .psram_ce_n(PREFIX``_ce_n), \
        .psram_clk(PREFIX``_clk) \
    )

    `PSRAM_DRIVER_INSTANCE(psram_driver_production, u_prod, prod);
    `PSRAM_DRIVER_INSTANCE(psram_driver_async_reset, u_async, async);
    `PSRAM_DRIVER_INSTANCE(psram_driver_sync_reset, u_sync, sync);
    `PSRAM_DRIVER_INSTANCE(psram_driver_no_iddr_reset, u_norst, norst);

`undef PSRAM_DRIVER_INSTANCE

    function automatic [31:0] repeated_nibble(input logic [3:0] value);
        repeated_nibble = {8{value}};
    endfunction

    integer read_sample_count = 0;
    always @(posedge clk) begin
        if (!resetn)
            read_sample_count = 0;
        else if (valid && async_ready)
            read_sample_count = 0;
        else if (u_async.sh_rd_en_tape[31])
            read_sample_count = read_sample_count + 1;
    end

    task automatic fail(input string message);
        begin
            $display("FAIL: %s", message);
            $finish;
        end
    endtask

    task automatic wait_all_ready;
        integer timeout;
        begin
            timeout = 0;
            while (!(prod_ready && async_ready && sync_ready && norst_ready)) begin
                @(negedge clk);
                timeout = timeout + 1;
                if (timeout > 100)
                    fail("driver variants did not become ready together");
            end
        end
    endtask

    task automatic start_read(input logic edge_select);
        begin
            wait_all_ready();
            dcount_edge = edge_select;
            cmd = 8'heb;
            addr = 24'h2468ac;
            wdata = 64'h0;
            valid = 1'b1;
            @(posedge clk);
            #1;
            if (!(prod_ready == 1'b0 && async_ready == 1'b0 &&
                  sync_ready == 1'b0 && norst_ready == 1'b0))
                fail("QPI read was not accepted by every variant");
            @(negedge clk);
            valid = 1'b0;
        end
    endtask

    task automatic start_command(input logic [7:0] command);
        begin
            wait_all_ready();
            cmd = command;
            addr = 24'h2468ac;
            wdata = 64'h0123456789abcdef;
            valid = 1'b1;
            @(posedge clk);
            #1;
            if (!(prod_ready == 1'b0 && async_ready == 1'b0 &&
                  sync_ready == 1'b0 && norst_ready == 1'b0))
                fail("command was not accepted by every variant");
            @(negedge clk);
            valid = 1'b0;
        end
    endtask

    task automatic finish_read(
        input logic edge_select,
        input logic [3:0] expected_a,
        input logic [3:0] expected_b,
        input string label
    );
        logic [63:0] expected;
        integer timeout;
        begin
            expected = {repeated_nibble(expected_b),
                        repeated_nibble(expected_a)};
            timeout = 0;
            while (!(prod_rvalid || async_rvalid || sync_rvalid || norst_rvalid)) begin
                @(negedge clk);
                timeout = timeout + 1;
                if (timeout > 100)
                    fail({label, ": no read response"});
            end
            if (!(prod_rvalid && async_rvalid && sync_rvalid && norst_rvalid))
                fail({label, ": variants asserted rvalid on different cycles"});
            if (prod_rdata !== expected)
                fail({label, ": production read data mismatch"});
            if (async_rdata !== expected)
                fail({label, ": async-reset read data mismatch"});
            if (sync_rdata !== expected)
                fail({label, ": sync-reset read data mismatch"});
            if (norst_rdata !== expected)
                fail({label, ": no-reset read data mismatch"});
            if (!(prod_rdata === async_rdata && async_rdata === sync_rdata &&
                  sync_rdata === norst_rdata))
                fail({label, ": read data differs between IDDR reset modes"});
            if (read_sample_count != 8)
                fail({label, ": QPI read did not capture exactly eight samples"});
            $display("PASS %s edge=%0d data=%016h", label, edge_select, expected);
            @(negedge clk);
        end
    endtask

    task automatic run_read(
        input logic edge_select,
        input logic [3:0] expected_a,
        input logic [3:0] expected_b,
        input string label
    );
        begin
            start_read(edge_select);
            finish_read(edge_select, expected_a, expected_b, label);
        end
    endtask

    task automatic assert_mid_command_reset;
        begin
            #1;
            resetn = 1'b0;
            valid = 1'b0;
            #1;
            if (u_async.a_pos !== 4'h0 || u_async.a_neg !== 4'h0 ||
                u_async.b_pos !== 4'h0 || u_async.b_neg !== 4'h0)
                fail("asynchronous IDDR reset did not clear between edges");
            if (u_sync.a_pos === 4'h0 && u_sync.a_neg === 4'h0 &&
                u_sync.b_pos === 4'h0 && u_sync.b_neg === 4'h0)
                fail("synchronous IDDR reset changed before a rising edge");

            @(posedge clk);
            #1;
            if (u_sync.a_pos !== 4'h0 || u_sync.a_neg !== 4'h0 ||
                u_sync.b_pos !== 4'h0 || u_sync.b_neg !== 4'h0)
                fail("synchronous IDDR reset did not clear on the rising edge");
            if (u_norst.a_pos === 4'h0 && u_norst.a_neg === 4'h0 &&
                u_norst.b_pos === 4'h0 && u_norst.b_neg === 4'h0)
                fail("no-reset IDDR unexpectedly cleared during reset");
            if (prod_rvalid || async_rvalid || sync_rvalid || norst_rvalid)
                fail("aborted command produced a response during reset");

            repeat (3) begin
                @(posedge clk);
                #1;
                if (prod_rvalid || async_rvalid || sync_rvalid || norst_rvalid)
                    fail("aborted command produced a late response");
            end
            if (!(prod_done && async_done && sync_done && norst_done))
                fail("reset did not clear the engine tapes");
            @(negedge clk);
            resetn = 1'b1;
        end
    endtask

    task automatic abort_read_with_reset(
        input logic edge_select,
        input integer reset_after_samples
    );
        integer samples;
        begin
            start_read(edge_select);
            samples = 0;
            while (samples < reset_after_samples) begin
                @(negedge clk);
                if (u_async.sh_rd_en_tape[31])
                    samples = samples + 1;
            end

            // Assert reset between clock edges. ASYNC
            // clears at once, SYNC clears on the next rising edge, and the
            // no-reset IDDR keeps sampling. The engine must cancel all three.
            assert_mid_command_reset();
            $display("PASS mid-read reset edge=%0d samples=%0d",
                     edge_select, reset_after_samples);
        end
    endtask

    task automatic abort_write_with_reset;
        begin
            start_command(8'h02);
            repeat (6) @(negedge clk);
            if (!u_async.sh_oe_tape[31])
                fail("write reset was not applied during the output window");
            assert_mid_command_reset();
            $display("PASS mid-write reset");
        end
    endtask

    task automatic check_delay_tap(input logic [4:0] tap);
        integer i;
        begin
            dcount_wr = tap;
            dcount_wr_en = 1'b1;
            @(posedge clk);
            @(negedge clk);
            #1;
            dcount_wr_en = 1'b0;
            for (i = 0; i < 4; i = i + 1) begin
                if (u_prod.a_cntvalue[i] !== tap ||
                    u_prod.b_cntvalue[i] !== tap ||
                    u_async.a_cntvalue[i] !== tap ||
                    u_async.b_cntvalue[i] !== tap ||
                    u_sync.a_cntvalue[i] !== tap ||
                    u_sync.b_cntvalue[i] !== tap ||
                    u_norst.a_cntvalue[i] !== tap ||
                    u_norst.b_cntvalue[i] !== tap)
                    begin
                        $display("FAIL: IDELAY tap load differs at tap=%0d lane=%0d",
                                 tap, i);
                        $display("prod=%0d/%0d async=%0d/%0d sync=%0d/%0d norst=%0d/%0d",
                                 u_prod.a_cntvalue[i], u_prod.b_cntvalue[i],
                                 u_async.a_cntvalue[i], u_async.b_cntvalue[i],
                                 u_sync.a_cntvalue[i], u_sync.b_cntvalue[i],
                                 u_norst.a_cntvalue[i], u_norst.b_cntvalue[i]);
                        $finish;
                    end
            end
            $display("PASS IDELAY tap %0d", tap);
            @(negedge clk);
        end
    endtask

    initial begin
        integer phase;
        integer sample_case;
        integer reset_samples [0:2];
        reset_samples[0] = 1;
        reset_samples[1] = 3;
        reset_samples[2] = 7;

        // glbl.GSR starts high in functional simulation. INIT_Q1/Q2 must
        // hold all three reset choices at zero before module reset releases.
        #2;
        if (u_prod.a_pos !== 4'h0 || u_prod.a_neg !== 4'h0 ||
            u_prod.b_pos !== 4'h0 || u_prod.b_neg !== 4'h0 ||
            u_async.a_pos !== 4'h0 || u_async.a_neg !== 4'h0 ||
            u_async.b_pos !== 4'h0 || u_async.b_neg !== 4'h0 ||
            u_sync.a_pos !== 4'h0 || u_sync.a_neg !== 4'h0 ||
            u_sync.b_pos !== 4'h0 || u_sync.b_neg !== 4'h0 ||
            u_norst.a_pos !== 4'h0 || u_norst.a_neg !== 4'h0 ||
            u_norst.b_pos !== 4'h0 || u_norst.b_neg !== 4'h0)
            fail("IDDR INIT/GSR outputs were not zero");
        // Xilinx functional glbl holds GSR for the first 100 ns.
        #105;
        repeat (2) @(posedge clk);
        @(negedge clk);
        resetn = 1'b1;

        check_delay_tap(5'd0);
        check_delay_tap(5'd15);
        check_delay_tap(5'd31);

        // Q1 and Q2 see distinct steady half-cycle values. These reads prove
        // the dcount_edge mux and all eight QPI nibbles in each 32-bit half.
        pos_a_drive = 4'h1;
        pos_b_drive = 4'h2;
        neg_a_drive = 4'ha;
        neg_b_drive = 4'hb;
        run_read(1'b0, 4'h1, 4'h2, "baseline positive-edge read");
        run_read(1'b1, 4'ha, 4'hb, "baseline negative-edge read");

        // Poison both IDDR phases, abort reads after 1, 3, and 7 samples,
        // then change the source values before the first post-reset read.
        for (phase = 0; phase < 2; phase = phase + 1) begin
            for (sample_case = 0; sample_case < 3;
                 sample_case = sample_case + 1) begin
                pos_a_drive = 4'hf;
                pos_b_drive = 4'hf;
                neg_a_drive = 4'hf;
                neg_b_drive = 4'hf;
                abort_read_with_reset(phase[0], reset_samples[sample_case]);
                pos_a_drive = phase ? 4'h5 : 4'h3;
                pos_b_drive = phase ? 4'h6 : 4'h4;
                neg_a_drive = phase ? 4'he : 4'hc;
                neg_b_drive = phase ? 4'h7 : 4'hd;
                if (phase == 0)
                    run_read(1'b0, 4'h3, 4'h4,
                             "immediate post-reset positive-edge read");
                else
                    run_read(1'b1, 4'he, 4'h7,
                             "immediate post-reset negative-edge read");
            end
        end

        abort_write_with_reset();
        pos_a_drive = 4'h8;
        pos_b_drive = 4'h9;
        neg_a_drive = 4'h6;
        neg_b_drive = 4'h7;
        run_read(1'b0, 4'h8, 4'h9, "clean read after write reset");

        $display("PSRAM IDDR RESET PASS");
        $finish;
    end

    initial begin
        #200000;
        fail("global timeout");
    end

endmodule
