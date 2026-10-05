// Output-resolution display masks, matching Multitini's RGB888 attenuation
// followed by RGB565 truncation. No line/frame store or ARM pixel work.
module video_pixel_mask (
    input logic pixel_clk, resetn,
    input logic [607:0] config_data,
    input logic [15:0] pixel_in,
    input logic [11:0] x, y,
    input logic de_in, hsync_in, vsync_in,
    output logic [15:0] pixel_out,
    output logic de_out, hsync_out, vsync_out
);
    wire [1:0] mode = config_data[1:0];
    wire [3:0] count = config_data[7:4];
    wire [11:0] x0 = config_data[32 +: 12], x1 = config_data[48 +: 12];
    wire [11:0] y0 = config_data[64 +: 12], y1 = config_data[80 +: 12];
    logic [7:0] excluded;
    integer n;
    always_comb begin
        for (n=0; n<8; n=n+1)
            excluded[n] = n < count &&
                x >= config_data[96+n*64 +: 12] &&
                x <  config_data[112+n*64 +: 12] &&
                y >= config_data[128+n*64 +: 12] &&
                y <  config_data[144+n*64 +: 12];
    end

    logic [11:0] local_x0, local_y0;
    logic [7:0] excluded0;
    logic viewport0;
    logic [15:0] pixels [0:2];
    logic [1:0] modes [0:2];
    logic [2:0] timing [0:2];
    logic enable1, enable2, y_odd1, y_odd2;
    logic [4:0] x_sum1, y_sum1;
    logic [1:0] x_phase2, y_phase2;

    // Since 4 == 1 (mod 3), six base-4 digits suffice. Pipeline the sum
    // separately from its bounded 0..18 remainder decoder at 148.5 MHz.
    function automatic [4:0] digit_sum(input [11:0] value);
        digit_sum = {3'b0,value[1:0]} + {3'b0,value[3:2]} +
                    {3'b0,value[5:4]} + {3'b0,value[7:6]} +
                    {3'b0,value[9:8]} + {3'b0,value[11:10]};
    endfunction
    function automatic [1:0] mod3(input [4:0] value);
        case (value)
            0,3,6,9,12,15,18: mod3 = 0;
            1,4,7,10,13,16: mod3 = 1;
            default: mod3 = 2;
        endcase
    endfunction
    function automatic [7:0] dim(input [7:0] value);
        dim = value - (value >> 2);
    endfunction
    wire [7:0] red = {pixels[2][15:11],pixels[2][15:13]};
    wire [7:0] green = {pixels[2][10:5],pixels[2][10:9]};
    wire [7:0] blue = {pixels[2][4:0],pixels[2][4:2]};
    wire [7:0] dim_red = dim(red), dim_green = dim(green), dim_blue = dim(blue);
    wire [1:0] selected = modes[2] == 2 && y_odd2 ?
        (x_phase2 == 2 ? 2'd0 : x_phase2 + 2'd1) : x_phase2;
    wire grid_dim = modes[2] == 3 && (x_phase2 == 2 || y_phase2 == 2);
    wire stripe = modes[2] == 1 || modes[2] == 2;
    wire dim_r = enable2 && (grid_dim || (stripe && selected != 0));
    wire dim_g = enable2 && (grid_dim || (stripe && selected != 1));
    wire dim_b = enable2 && (grid_dim || (stripe && selected != 2));

    integer s;
    always_ff @(posedge pixel_clk) begin
        if (!resetn) begin
            for (s=0; s<3; s=s+1) begin
                pixels[s] <= 0; modes[s] <= 0; timing[s] <= 0;
            end
            local_x0 <= 0; local_y0 <= 0; excluded0 <= 0; viewport0 <= 0;
            enable1 <= 0; enable2 <= 0; y_odd1 <= 0; y_odd2 <= 0;
            x_sum1 <= 0; y_sum1 <= 0; x_phase2 <= 0; y_phase2 <= 0;
            pixel_out <= 0; de_out <= 0; hsync_out <= 0; vsync_out <= 0;
        end else begin
            local_x0 <= x - x0; local_y0 <= y - y0;
            excluded0 <= excluded;
            viewport0 <= x >= x0 && x < x1 && y >= y0 && y < y1;
            pixels[0] <= pixel_in; modes[0] <= mode;
            timing[0] <= {de_in,hsync_in,vsync_in};
            for (s=1; s<3; s=s+1) begin
                pixels[s] <= pixels[s-1]; modes[s] <= modes[s-1];
                timing[s] <= timing[s-1];
            end
            enable1 <= viewport0 && !(|excluded0);
            enable2 <= enable1;
            y_odd1 <= local_y0[0]; y_odd2 <= y_odd1;
            x_sum1 <= digit_sum(local_x0); y_sum1 <= digit_sum(local_y0);
            x_phase2 <= mod3(x_sum1); y_phase2 <= mod3(y_sum1);
            pixel_out <= {dim_r ? dim_red[7:3] : pixels[2][15:11],
                          dim_g ? dim_green[7:2] : pixels[2][10:5],
                          dim_b ? dim_blue[7:3] : pixels[2][4:0]};
            {de_out,hsync_out,vsync_out} <= timing[2];
        end
    end
endmodule
