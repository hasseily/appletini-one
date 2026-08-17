`timescale 1ns / 1ps

module tb_uthernet2_host_fifo;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic addr_we = 1'b0;
    logic [15:0] addr_wdata = 16'h0000;
    logic control_we = 1'b0;
    logic [31:0] control_wdata = 32'h0000_0000;
    logic data_we = 1'b0;
    logic [31:0] data_wdata = 32'h0000_0000;
    logic data_re = 1'b0;
    logic [31:0] data_rdata;
    logic [31:0] status;
    logic direct_busy = 1'b0;
    logic ready_enable = 1'b1;
    logic host_ready;
    logic host_done = 1'b0;
    logic host_error = 1'b0;
    logic [7:0] host_rdata = 8'h00;
    logic host_req;
    logic host_write;
    logic [15:0] host_addr;
    logic [7:0] host_wdata;
    logic busy;

    logic backend_busy = 1'b0;
    logic inject_error = 1'b0;
    logic [1:0] backend_wait = 2'd0;
    logic [15:0] backend_addr = 16'h0000;
    integer request_count = 0;
    logic request_write [0:1023];
    logic [15:0] request_addr [0:1023];
    logic [7:0] request_data [0:1023];

    always #5 clk = ~clk;
    assign host_ready = ready_enable && !backend_busy;

    uthernet2_host_fifo dut (
        .clk(clk),
        .rstn(rstn),
        .addr_we(addr_we),
        .addr_wdata(addr_wdata),
        .control_we(control_we),
        .control_wdata(control_wdata),
        .data_we(data_we),
        .data_wdata(data_wdata),
        .data_re(data_re),
        .data_rdata(data_rdata),
        .status(status),
        .direct_busy(direct_busy),
        .host_ready(host_ready),
        .host_done(host_done),
        .host_error(host_error),
        .host_rdata(host_rdata),
        .host_req(host_req),
        .host_write(host_write),
        .host_addr(host_addr),
        .host_wdata(host_wdata),
        .busy(busy)
    );

    always_ff @(posedge clk) begin
        host_done <= 1'b0;
        host_error <= 1'b0;
        if (!rstn) begin
            backend_busy <= 1'b0;
            backend_wait <= 2'd0;
            backend_addr <= 16'h0000;
            request_count <= 0;
        end else if (host_req && host_ready) begin
            request_write[request_count] <= host_write;
            request_addr[request_count] <= host_addr;
            request_data[request_count] <= host_wdata;
            request_count <= request_count + 1;
            backend_addr <= host_addr;
            backend_wait <= 2'd2;
            backend_busy <= 1'b1;
        end else if (backend_busy) begin
            if (backend_wait == 0) begin
                backend_busy <= 1'b0;
                host_done <= 1'b1;
                host_error <= inject_error;
                host_rdata <= backend_addr[7:0] ^ 8'hA5;
            end else begin
                backend_wait <= backend_wait - 2'd1;
            end
        end
    end

    task automatic write_addr(input logic [15:0] value);
        begin
            @(negedge clk);
            addr_wdata = value;
            addr_we = 1'b1;
            @(negedge clk);
            addr_we = 1'b0;
        end
    endtask

    task automatic write_control(input logic [31:0] value);
        begin
            @(negedge clk);
            control_wdata = value;
            control_we = 1'b1;
            @(negedge clk);
            control_we = 1'b0;
        end
    endtask

    task automatic write_data(input logic [31:0] value);
        begin
            @(negedge clk);
            data_wdata = value;
            data_we = 1'b1;
            @(negedge clk);
            data_we = 1'b0;
        end
    endtask

    task automatic read_data(output logic [31:0] value);
        begin
            @(negedge clk);
            value = data_rdata;
            data_re = 1'b1;
            @(negedge clk);
            data_re = 1'b0;
        end
    endtask

    task automatic wait_done;
        integer polls;
        begin
            polls = 0;
            while (!status[30] && polls < 2000) begin
                @(posedge clk);
                polls = polls + 1;
            end
            if (!status[30]) begin
                $fatal(1, "FIFO operation did not finish");
            end
        end
    endtask

    integer base_count;
    integer i;
    logic [31:0] word;
    initial begin
        repeat (4) @(posedge clk);
        rstn = 1'b1;
        repeat (2) @(posedge clk);

        write_control(32'h2000_0000);
        write_addr(16'h1234);
        write_data(32'h4433_2211);
        write_data(32'h8877_6655);
        base_count = request_count;
        write_control(32'hC000_0007);
        wait_done();
        if (status[29] || request_count != base_count + 7) begin
            $fatal(1, "seven-byte FIFO write failed");
        end
        for (i = 0; i < 7; i = i + 1) begin
            if (!request_write[base_count + i] ||
                request_addr[base_count + i] != 16'h1234 + i ||
                request_data[base_count + i] != 8'h11 + (i * 8'h11)) begin
                $fatal(1, "FIFO write request mismatch at byte %0d", i);
            end
        end

        write_control(32'h2000_0000);
        write_addr(16'h2000);
        base_count = request_count;
        write_control(32'h8000_0007);
        wait_done();
        if (status[29] || status[17:9] != 9'd7 ||
            request_count != base_count + 7) begin
            $fatal(1, "seven-byte FIFO read failed");
        end
        read_data(word);
        if (word != 32'hA6A7_A4A5) begin
            $fatal(1, "first packed FIFO read word mismatch: %08x", word);
        end
        read_data(word);
        if (word != 32'h00A3_A0A1 || status[17:9] != 9'd0) begin
            $fatal(1, "second packed FIFO read word mismatch: %08x", word);
        end

        write_control(32'h2000_0000);
        write_addr(16'h5000);
        for (i = 0; i < 64; i = i + 1) begin
            word[7:0] = (i * 4);
            word[15:8] = (i * 4) + 1;
            word[23:16] = (i * 4) + 2;
            word[31:24] = (i * 4) + 3;
            write_data(word);
        end
        base_count = request_count;
        write_control(32'hC000_0100);
        wait_done();
        if (status[29] || request_count != base_count + 256) begin
            $fatal(1, "full 256-byte FIFO write failed");
        end
        for (i = 0; i < 256; i = i + 1) begin
            if (!request_write[base_count + i] ||
                request_addr[base_count + i] != 16'h5000 + i ||
                request_data[base_count + i] != (i & 8'hFF)) begin
                $fatal(1, "full FIFO write mismatch at byte %0d", i);
            end
        end

        write_control(32'h2000_0000);
        write_addr(16'h6000);
        base_count = request_count;
        write_control(32'h8000_0100);
        wait_done();
        if (status[29] || status[17:9] != 9'd256 ||
            request_count != base_count + 256) begin
            $fatal(1, "full 256-byte FIFO read failed");
        end
        for (i = 0; i < 64; i = i + 1) begin
            read_data(word);
            if (word[7:0] != (((i * 4) + 0) ^ 8'hA5) ||
                word[15:8] != (((i * 4) + 1) ^ 8'hA5) ||
                word[23:16] != (((i * 4) + 2) ^ 8'hA5) ||
                word[31:24] != (((i * 4) + 3) ^ 8'hA5)) begin
                $fatal(1, "full FIFO read mismatch at word %0d", i);
            end
        end
        if (status[17:9] != 9'd0) begin
            $fatal(1, "full FIFO read did not drain");
        end

        write_control(32'h2000_0000);
        direct_busy = 1'b1;
        base_count = request_count;
        write_control(32'h8000_0001);
        if (!status[30] || !status[29] || request_count != base_count) begin
            $fatal(1, "direct-busy FIFO start was not rejected");
        end
        direct_busy = 1'b0;

        write_control(32'h2000_0000);
        write_data(32'h0403_0201);
        write_control(32'hC000_0005);
        if (!status[30] || !status[29]) begin
            $fatal(1, "short staged write was not rejected");
        end

        write_control(32'h2000_0000);
        write_addr(16'h3000);
        ready_enable = 1'b0;
        base_count = request_count;
        write_control(32'h8000_0002);
        repeat (8) @(posedge clk);
        if (request_count != base_count || !busy) begin
            $fatal(1, "FIFO did not wait for host_ready");
        end
        ready_enable = 1'b1;
        wait_done();
        if (status[29] || request_count != base_count + 2) begin
            $fatal(1, "FIFO did not resume after host_ready stall");
        end

        write_control(32'h2000_0000);
        write_addr(16'h4000);
        inject_error = 1'b1;
        base_count = request_count;
        write_control(32'h8000_0003);
        wait_done();
        if (!status[29] || request_count != base_count + 1 || busy) begin
            $fatal(1, "host error did not stop the FIFO transfer");
        end
        inject_error = 1'b0;

        $display("UTHERNET2 HOST FIFO PASS");
        $finish;
    end

endmodule
