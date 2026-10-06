`timescale 1ns / 1ps

// Post-engine SSI balance. Volume never changes excitation or tract state.
// Each engine owns its sample-valid timing; the mixer cannot stall either one.
// Four combined gain/pan coefficients share one DSP at the fabric clock. An
// 18-clock job publishes both channels together. New samples received during
// a job remain pending and start another job with the latest held values.
module ssi263_stereo_mixer (
    input  logic               clk,
    input  logic               rstn,
    input  logic               audio_tick,
    input  logic signed [15:0] sample0,
    input  logic signed [15:0] sample1,
    input  logic               sample0_valid,
    input  logic               sample1_valid,
    input  logic signed [4:0]  volume_db,
    // Low nibble = secondary SSI, high nibble = primary SSI; 0 left, 15 right.
    input  logic [7:0]         pan,
    output logic signed [17:0] speech_l,
    output logic signed [17:0] speech_r,
    output logic               mix_valid
);
    localparam logic [2:0] IDLE=0, LOAD=1, MULTIPLY=2, ABSOLUTE=3, STORE=4, PUBLISH=5;
    localparam logic [15:0] DEFAULT_GAIN = 16'd20626; // +2 dB in Q14.
    localparam logic [15:0] RAMP_STEP = 16'd64;
    logic [2:0] state_q;
    logic [1:0] lane_q;
    logic pending_q;
    logic signed [15:0] held0_q, held1_q, snapshot0_q, snapshot1_q;
    // Lane order: socket0-left, socket0-right, socket1-left, socket1-right.
    logic [15:0] gain_q [0:3], target_gain [0:3], target_gain_q [0:3], next_gain [0:3], snapshot_gain_q [0:3];
    logic [15:0] volume_gain_q;
    logic signed [15:0] multiply_sample_q, multiply_gain_q;
    (* use_dsp = "yes" *) logic signed [31:0] product_q;
    logic [31:0] magnitude_q, rounded_magnitude;
    logic negative_q;
    logic signed [17:0] scaled, terms_q [0:3];
    integer i;

    // round(16384 * 10^(dB/20)); clamp malformed signed control fields.
    function automatic logic [15:0] db_gain(input logic signed [4:0] db);
        if (db < -5'sd5) db_gain=16'd9213;
        else if (db > 5'sd5) db_gain=16'd29135;
        else case (db)
            -5'sd5: db_gain=16'd9213;
            -5'sd4: db_gain=16'd10338;
            -5'sd3: db_gain=16'd11599;
            -5'sd2: db_gain=16'd13014;
            -5'sd1: db_gain=16'd14602;
             5'sd0: db_gain=16'd16384;
             5'sd1: db_gain=16'd18383;
             5'sd2: db_gain=16'd20626;
             5'sd3: db_gain=16'd23143;
             5'sd4: db_gain=16'd25967;
             default: db_gain=16'd29135;
        endcase
    endfunction
    // Same balance law as the AY mixer. Pan 8 gives both sides full level;
    // moving from one edge to the other fades the opposite side in/out.
    function automatic logic [4:0] pan_left(input logic [3:0] value);
        case (value)
            9:pan_left=14; 10:pan_left=11; 11:pan_left=9;
            12:pan_left=7; 13:pan_left=5; 14:pan_left=2; 15:pan_left=0;
            default:pan_left=16;
        endcase
    endfunction
    function automatic logic [4:0] pan_right(input logic [3:0] value);
        case (value)
            0:pan_right=0; 1:pan_right=2; 2:pan_right=4; 3:pan_right=6;
            4:pan_right=8; 5:pan_right=10; 6:pan_right=12; 7:pan_right=14;
            default:pan_right=16;
        endcase
    endfunction
    // A small shift/add selector avoids using DSPs for control coefficients.
    // Fractional rounding here is below one Q14 coefficient unit per term.
    function automatic logic [15:0] pan_coefficient(input logic [15:0] gain, input logic [4:0] balance);
        case (balance)
            0:pan_coefficient=0;
            2:pan_coefficient=gain>>3;
            4:pan_coefficient=gain>>2;
            5:pan_coefficient=(gain>>2)+(gain>>4);
            6:pan_coefficient=(gain>>2)+(gain>>3);
            7:pan_coefficient=(gain>>2)+(gain>>3)+(gain>>4);
            8:pan_coefficient=gain>>1;
            9:pan_coefficient=(gain>>1)+(gain>>4);
            10:pan_coefficient=(gain>>1)+(gain>>3);
            11:pan_coefficient=(gain>>1)+(gain>>3)+(gain>>4);
            12:pan_coefficient=(gain>>1)+(gain>>2);
            14:pan_coefficient=(gain>>1)+(gain>>2)+(gain>>3);
            default:pan_coefficient=gain;
        endcase
    endfunction
    function automatic logic [15:0] ramp(input logic [15:0] current_gain, input logic [15:0] target);
        if (current_gain < target)
            ramp=(target-current_gain > RAMP_STEP) ? current_gain+RAMP_STEP : target;
        else
            ramp=(current_gain-target > RAMP_STEP) ? current_gain-RAMP_STEP : target;
    endfunction

    always_comb begin
        target_gain[0]=pan_coefficient(volume_gain_q,pan_left(pan[3:0]));
        target_gain[1]=pan_coefficient(volume_gain_q,pan_right(pan[3:0]));
        target_gain[2]=pan_coefficient(volume_gain_q,pan_left(pan[7:4]));
        target_gain[3]=pan_coefficient(volume_gain_q,pan_right(pan[7:4]));
        for (integer k=0;k<4;k=k+1) next_gain[k]=ramp(gain_q[k],target_gain_q[k]);
        rounded_magnitude=magnitude_q+32'd8192;
        scaled=negative_q ? -$signed(rounded_magnitude[31:14]) : $signed(rounded_magnitude[31:14]);
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q<=IDLE; lane_q<=0; pending_q<=0;
            held0_q<=0; held1_q<=0; snapshot0_q<=0; snapshot1_q<=0;
            speech_l<=0; speech_r<=0; mix_valid<=0;
            // Match the saved UI defaults without an initial fade from zero.
            gain_q[0]<=DEFAULT_GAIN; gain_q[1]<=0;
            gain_q[2]<=0; gain_q[3]<=DEFAULT_GAIN;
            target_gain_q[0]<=DEFAULT_GAIN; target_gain_q[1]<=0;
            target_gain_q[2]<=0; target_gain_q[3]<=DEFAULT_GAIN;
            volume_gain_q<=DEFAULT_GAIN;
        end else begin
            mix_valid<=0;
            // Register control conversion separately from the ramp so its
            // ROM, pan sums and bounded step are not one fabric-clock path.
            volume_gain_q<=db_gain(volume_db);
            for (i=0;i<4;i=i+1) target_gain_q[i]<=target_gain[i];
            if (sample0_valid) begin held0_q<=sample0; pending_q<=1; end
            if (sample1_valid) begin held1_q<=sample1; pending_q<=1; end
            if (audio_tick) begin
                for (i=0;i<4;i=i+1) gain_q[i]<=next_gain[i];
                pending_q<=1;
            end
            case (state_q)
                IDLE: if (pending_q || sample0_valid || sample1_valid || audio_tick) begin
                    // Include a fresh valid/tick arriving on the snapshot
                    // clock so clearing pending cannot discard that update.
                    snapshot0_q<=sample0_valid ? sample0 : held0_q;
                    snapshot1_q<=sample1_valid ? sample1 : held1_q;
                    for (i=0;i<4;i=i+1) snapshot_gain_q[i]<=audio_tick ? next_gain[i] : gain_q[i];
                    pending_q<=0; lane_q<=0; state_q<=LOAD;
                end
                LOAD: begin
                    multiply_sample_q<=lane_q[1] ? snapshot1_q : snapshot0_q;
                    multiply_gain_q<=$signed(snapshot_gain_q[lane_q]);
                    state_q<=MULTIPLY;
                end
                MULTIPLY: begin product_q<=multiply_sample_q*multiply_gain_q; state_q<=ABSOLUTE; end
                ABSOLUTE: begin
                    magnitude_q<=product_q[31] ? $unsigned(-product_q) : $unsigned(product_q);
                    negative_q<=product_q[31]; state_q<=STORE;
                end
                STORE: begin
                    terms_q[lane_q]<=scaled;
                    if (lane_q==3) state_q<=PUBLISH;
                    else begin lane_q<=lane_q+1'b1; state_q<=LOAD; end
                end
                PUBLISH: begin
                    // ±116540 fits signed18 even with both sources at +5 dB.
                    // Leave headroom for AY; the caller performs the final clamp.
                    speech_l<=terms_q[0]+terms_q[2];
                    speech_r<=terms_q[1]+terms_q[3];
                    mix_valid<=1; state_q<=IDLE;
                end
                default: state_q<=IDLE;
            endcase
        end
    end
endmodule
