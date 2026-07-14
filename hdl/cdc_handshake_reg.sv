module cdc_handshake_reg #(
        parameter integer WIDTH = 32
) (
    input wire wr_clk,
    input wire wr_rstn,
    input wire [WIDTH-1:0] din,
    input wire resend,
    input wire rd_clk,
    input wire rd_rstn,
    output wire [WIDTH-1:0] dout
);

wire dest_req;
reg src_send;
wire src_rcv;
wire [WIDTH-1:0] xpm_dout;

reg [WIDTH-1:0] last_send = 0;
reg [WIDTH-1:0] last_recv = 0;

assign dout = last_recv;

// Instantiate the xpm_cdc_handshake macro
xpm_cdc_handshake #(
    .DEST_EXT_HSK(0),   // 1 = Use external handshake (dest_ack), 0 = Internal
    .WIDTH(WIDTH)          // Width of the data bus
) xpm_cdc_handshake_inst (
    .dest_ack(0),
    .dest_clk(rd_clk),    // Destination Clock
    .dest_req(dest_req),    // Output: Data Valid in Dest Domain
    .dest_out(xpm_dout),    // Output: Synchronized Data
    .src_clk(wr_clk),      // Source Clock
    .src_in(last_send),        // Input: Data to transfer
    .src_rcv(src_rcv),      // Output: Data received ACK
    .src_send(src_send)     // Input: Start Transfer
);

localparam WR_IDLE = 1'd0;
localparam WR_SEND = 1'd1;
reg wr_state;

always @(posedge wr_clk) begin
    if (!wr_rstn) begin
        src_send <= 0;
        last_send <= 0;
        wr_state <= WR_IDLE;
    end else begin
        case (wr_state)
            WR_IDLE: begin
                src_send <= 0;
                if (!src_rcv && (resend || (din != last_send))) begin
                    last_send <= din;
                    wr_state <= WR_SEND;
                end
            end
            WR_SEND: begin
                src_send <= 1;
                if (src_rcv) begin
                    wr_state <= WR_IDLE;
                end
            end
        endcase
    end
end

always @(posedge rd_clk) begin
    if (!rd_rstn) begin
        last_recv <= 0;
    end else begin
        if (dest_req) begin
            last_recv <= xpm_dout;
        end
    end 
end

endmodule
