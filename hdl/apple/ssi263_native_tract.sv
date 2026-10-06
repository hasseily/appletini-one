`timescale 1ns / 1ps

// Scheduled, ideal switched-capacitor candidate for the SC-02 prototype.
// Exact integer reference: scripts/ssi263_host/prototype_tract.cpp.
// This preserves that experiment; it does not establish final SSI-263 silicon
// behavior. Source drive trims arrive upstream. OUTPUT_GAIN is a listening
// setting, not a measured chip or potentiometer gain.
//
// One complete event is accepted when event_valid && event_ready. Changes to
// codes/sources/routes during a held phase are events too. The longest event
// takes 111 clocks after acceptance; effective XCK must allow at least 112.
// An event presented while busy sets fault and is not queued. No clock stalls
// or event drops are permitted in the caller's normal operating schedule.
module ssi263_native_tract #(
    parameter integer OUTPUT_GAIN = 8
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               event_valid,
    output logic               event_ready,
    input  logic               phase,
    input  logic               phase_edge,
    // Low-to-high nibbles: F1 F2 F2Q F3 F4 FILTERAMP VA FA.
    input  logic [31:0]        codes,
    input  logic signed [23:0] voice_drive,
    input  logic signed [17:0] fric_drive,
    input  logic               fric1_route,
    input  logic               fric2_route,
    input  logic               output_open,
    output logic               busy,
    output logic               event_done,
    output logic signed [23:0] reconstruction,
    output logic signed [15:0] sample,
    output logic               fault,
    output logic               state_saturated
);
    logic phase_q, phi0_edge_q, phi1_edge_q, route1_q, route2_q, output_open_q;
    logic [31:0] codes_q;
    logic [3:0] old_f2q_q;
    logic signed [23:0] voice_drive_q;
    logic signed [17:0] fric_drive_q, old_fric_q;
    logic signed [23:0] voice_q, fric1_q, fric2_source_q, fric2_shape_q;
    logic signed [23:0] voice_plates [0:3], fric1_plates [0:3];
    logic signed [23:0] f2q_plates [0:3], filter_plates [0:3];
    logic signed [23:0] f_y [0:4], f_p [0:4], f_fixed [0:4];
    logic signed [23:0] f_plates [0:4][0:3];
    logic signed [23:0] c143_plate_q, c151_plate_q;
    logic signed [23:0] c150_delta_q;
    logic signed [24:0] c151_delta_q;
    logic signed [23:0] output_q, phase_delta_q, edge_delta_q, old_voice_q, old_f1_q;

    // Six shared raw-pF multiply lanes, followed by an exact finite-divisor
    // reciprocal and correction. All capacitor differences need 25 bits.
    logic [3:0] stage_q;
    logic [3:0] step_q;
    localparam logic [3:0] LOAD=0, MUL=1, PARTIAL=2, SUM=3, ROUND=4, RECIP=5,
        QPLUS=6, CORRECT=7, RESOLVE=8, COMMIT=9, RETIRE=10;
    logic signed [24:0] operand [0:5], operand_q [0:5];
    logic signed [15:0] coefficient [0:5], coefficient_q [0:5];
    logic signed [40:0] product_q [0:5];
    logic signed [47:0] partial_q [0:2];
    logic signed [47:0] sum, numerator_q, rounded;
    logic [13:0] denominator, denominator_q;
    logic [47:0] absolute_value;
    logic [36:0] absolute_q;
    logic negative_q, zero_q, overflow_q, divisor_valid, divisor_valid_q;
    logic [25:0] reciprocal, reciprocal_q;
    logic [62:0] reciprocal_product_q;
    logic [23:0] quotient_q;
    logic [24:0] correction_operand_q;
    logic [38:0] correction_product_q;
    logic [23:0] corrected_magnitude;
    logic signed [23:0] result_comb, result;
    logic division_clipped_q;
    logic signed [23:0] update_base, update_base_q;
    logic update_subtract, update_subtract_q;
    logic signed [24:0] updated;
    logic signed [47:0] pcm_scaled;
    logic changed;
    logic [3:0] new_f2q, keep_f2q;
    integer i, j;

    function automatic logic signed [23:0] sat25(input logic signed [24:0] value);
        if (value[24] != value[23]) sat25 = value[24] ? 24'sh800000 : 24'sh7fffff;
        else sat25 = value[23:0];
    endfunction
    function automatic logic signed [15:0] cap(input integer bank, input integer bit_index);
        case (bank)
            0: case (bit_index) 0:cap=220; 1:cap=430; 2:cap=870; default:cap=1800; endcase
            1: case (bit_index) 0:cap=270; 1:cap=512; 2:cap=1068; default:cap=2160; endcase
            2: case (bit_index) 0:cap=270; 1:cap=530; 2:cap=1082; default:cap=2160; endcase
            3: case (bit_index) 0:cap=160; 1:cap=330; 2:cap=660; default:cap=1300; endcase
            4: case (bit_index) 0:cap=280; 1:cap=560; 2:cap=1120; default:cap=2300; endcase
            5: case (bit_index) 0:cap=210; 1:cap=420; 2:cap=820; default:cap=1640; endcase
            6: case (bit_index) 0:cap=200; 1:cap=400; 2:cap=820; default:cap=1620; endcase
            default: case (bit_index) 0:cap=76; 1:cap=150; 2:cap=300; default:cap=600; endcase
        endcase
    endfunction
    function automatic logic [13:0] cap_sum(input logic [3:0] mask, input integer bank);
        logic [13:0] total;
        total = 0;
        for (integer k=0; k<4; k=k+1) if (mask[k]) total = total + 14'(cap(bank,k));
        cap_sum = total;
    endfunction

    assign event_ready = !busy;
    assign changed = phase_edge || phase != phase_q || codes != codes_q ||
        voice_drive != voice_drive_q || fric_drive != fric_drive_q ||
        fric1_route != route1_q || fric2_route != route2_q;
    assign new_f2q = ~old_f2q_q & codes_q[11:8];
    assign keep_f2q = old_f2q_q & codes_q[11:8];
    always_comb begin
        pcm_scaled = ($signed(reconstruction) * 48'(OUTPUT_GAIN)) >>> 1;
        if (pcm_scaled > 48'sd32767) sample = 16'sh7fff;
        else if (pcm_scaled < -48'sd32768) sample = 16'sh8000;
        else sample = pcm_scaled[15:0];
    end

    // Operand selection uses only settled event/charge registers. The six
    // products are summed before a single nearest-away rounding operation.
    always_comb begin
        for (integer k=0; k<6; k=k+1) begin operand[k]=0; coefficient[k]=0; end
        denominator = 14'd1;
        case (stage_q)
            0: begin
                denominator=3900;
                if (phi0_edge_q) begin operand[0]=25'(fric2_shape_q); coefficient[0]=3600; end
                else if (phi1_edge_q) begin operand[0]=25'(fric2_source_q); coefficient[0]=-3600; end
            end
            1: begin denominator=3900; operand[0]=25'(phase_delta_q); coefficient[0]=-5700; end
            2: begin
                denominator=3900;
                operand[0]=25'(fric_drive_q)-25'(old_fric_q);
                coefficient[0]=-$signed({2'b0,cap_sum(codes_q[31:28],2)});
            end
            3: begin
                denominator=3900; operand[0]=25'(edge_delta_q);
                coefficient[0]=phase_q ? -9300 : -5700;
            end
            4: begin
                denominator=phase_q ? 3300 : 3900;
                for (integer k=0; k<4; k=k+1) begin
                    if (phase_q && codes_q[24+k]) begin
                        operand[k]=$signed(voice_drive_q)-$signed(voice_plates[k]); coefficient[k]=cap(0,k);
                    end else if (!phase_q && codes_q[28+k]) begin
                        operand[k]=25'(fric_drive_q)-$signed(fric1_plates[k]); coefficient[k]=cap(1,k);
                    end
                end
            end
            5: begin
                if (phase_q) begin
                    denominator=11700;
                    if (phi1_edge_q) begin
                        operand[0]=25'(f_p[0]); coefficient[0]=11500;
                        operand[1]=$signed(f_y[0])-$signed(voice_q); coefficient[1]=2700;
                        operand[2]=25'(voice_q); coefficient[2]=-2700;
                    end else begin operand[0]=$signed(voice_q)-$signed(old_voice_q); coefficient[0]=-5400; end
                end else if (phi0_edge_q) begin
                    denominator=7000+cap_sum(codes_q[11:8],0);
                    operand[0]=25'(f_p[1]); coefficient[0]=6800;
                    operand[1]=$signed(f_y[1])-$signed(f_y[0]); coefficient[1]=4700;
                    for (integer k=0; k<4; k=k+1) if (codes_q[8+k]) begin
                        operand[k+2]=25'(f2q_plates[k]); coefficient[k+2]=cap(0,k);
                    end
                end else if (new_f2q != 0) begin
                    denominator=7000+cap_sum(codes_q[11:8],0);
                    operand[0]=25'(f_p[1]); coefficient[0]=$signed({2'b0,14'd7000+cap_sum(keep_f2q,0)});
                    for (integer k=0; k<4; k=k+1) if (new_f2q[k]) begin
                        operand[k+1]=25'(f2q_plates[k]); coefficient[k+1]=cap(0,k);
                    end
                end
            end
            6: begin
                denominator=phase_q ? 11500 : 6800;
                operand[0]=phase_q ? $signed(f_p[0])-$signed(f_fixed[0]) : $signed(f_p[1])-$signed(f_fixed[1]);
                coefficient[0]=phase_q ? 250 : 500;
                for (integer k=0; k<4; k=k+1) begin
                    if (phase_q && codes_q[k]) begin operand[k+1]=$signed(f_p[0])-$signed(f_plates[0][k]); coefficient[k+1]=cap(3,k); end
                    else if (!phase_q && codes_q[4+k]) begin operand[k+1]=$signed(f_p[1])-$signed(f_plates[1][k]); coefficient[k+1]=cap(4,k); end
                end
                if (!phase_q && route1_q) begin operand[5]=$signed(c143_plate_q)-$signed(fric1_q); coefficient[5]=-1000; end
            end
            7: begin
                if (phase_q) begin
                    denominator=4900;
                    operand[2]=$signed(old_f1_q)-$signed(f_y[0]); coefficient[2]=2000;
                    if (phi1_edge_q) begin
                        operand[0]=25'(f_p[2]); coefficient[0]=4700;
                        operand[1]=$signed(f_y[2])-$signed(f_y[1]); coefficient[1]=3900;
                    end
                end else if (phi0_edge_q) begin
                    denominator=4500; operand[0]=25'(f_p[3]); coefficient[0]=4300;
                    operand[1]=$signed(f_y[3])-$signed(f_y[2]); coefficient[1]=4700;
                end
            end
            8: begin
                denominator=phase_q ? 4700 : 4300;
                operand[0]=phase_q ? $signed(f_p[2])-$signed(f_fixed[2]) : $signed(f_p[3])-$signed(f_fixed[3]);
                coefficient[0]=phase_q ? 820 : 1670;
                for (integer k=0; k<4; k=k+1) begin
                    if (phase_q && codes_q[12+k]) begin operand[k+1]=$signed(f_p[2])-$signed(f_plates[2][k]); coefficient[k+1]=cap(5,k); end
                    else if (!phase_q && codes_q[16+k]) begin operand[k+1]=$signed(f_p[3])-$signed(f_plates[3][k]); coefficient[k+1]=cap(6,k); end
                end
            end
            9: begin
                denominator=3730;
                operand[2]=25'(c150_delta_q); coefficient[2]=-1150;
                operand[3]=c151_delta_q; coefficient[3]=-3700;
                if (phi1_edge_q) begin
                    operand[0]=25'(f_p[4]); coefficient[0]=3450;
                    operand[1]=$signed(f_y[4])-$signed(f_y[3]); coefficient[1]=4700;
                end
            end
            10: begin denominator=3450; operand[0]=$signed(f_p[4])-$signed(f_fixed[4]); coefficient[0]=4700; end
            11: begin
                denominator=2750;
                if (phi1_edge_q) begin operand[0]=25'(output_q); coefficient[0]=2700; end
                for (integer k=0; k<4; k=k+1) if (codes_q[20+k]) begin
                    operand[k+1]=$signed(f_y[4])-$signed(filter_plates[k]);
                    coefficient[k+1]=phi1_edge_q ? -cap(7,k) : cap(7,k);
                end
            end
            default: begin end
        endcase
        sum=0;
        for (integer k=0; k<3; k=k+1) sum=sum+partial_q[k];
    end

    // For rounded magnitude n < d*2^23 and d < 2^14, floor(n*M/2^37)
    // is floor(n/d) or one less when M=floor(2^37/d). The correction multiply
    // chooses the exact quotient. Saturation happens before reciprocal use.
    always_comb begin
        rounded = numerator_q < 0 ? numerator_q-$signed({35'd0,denominator_q[13:1]}) :
            numerator_q+$signed({35'd0,denominator_q[13:1]});
        absolute_value=rounded[47] ? $unsigned(-rounded) : $unsigned(rounded);
        divisor_valid=1;
        case (denominator_q)
            1: reciprocal=0; // Zero numerator in an inactive stage.
            2750:reciprocal=26'h2FA99C9; 3300:reciprocal=26'h27B8027;
            3450:reciprocal=26'h25FDEC1; 3730:reciprocal=26'h2323D38;
            3900:reciprocal=26'h219BB35; 4300:reciprocal=26'h1E7B5B3;
            4500:reciprocal=26'h1D208A5; 4700:reciprocal=26'h1BE33DA;
            4900:reciprocal=26'h1ABFD7E; 6800:reciprocal=26'h134679A;
            7000:reciprocal=26'h12B97D8; 7220:reciprocal=26'h12276DA;
            7430:reciprocal=26'h11A4130; 7650:reciprocal=26'h1122334;
            7870:reciprocal=26'h10A7965; 8090:reciprocal=26'h1033A49;
            8300:reciprocal=26'h0FCAB3E; 8520:reciprocal=26'h0F62504;
            8800:reciprocal=26'h0EE500E; 9020:reciprocal=26'h0E8800E;
            9230:reciprocal=26'h0E335DC; 9450:reciprocal=26'h0DDEBBC;
            9670:reciprocal=26'h0D8DF39; 9890:reciprocal=26'h0D40C37;
            10100:reciprocal=26'h0CFA389; 10320:reciprocal=26'h0CB3660;
            11500:reciprocal=26'h0B65C6D; 11700:reciprocal=26'h0B33E67;
            default: begin reciprocal=0; divisor_valid=0; end
        endcase
        corrected_magnitude=quotient_q;
        if (correction_product_q <= {2'b00,absolute_q}) corrected_magnitude=quotient_q+24'd1;
        if (zero_q) result_comb=0;
        else if (overflow_q) result_comb=negative_q ? 24'sh800000 : 24'sh7fffff;
        else if (!divisor_valid_q) result_comb=0;
        else result_comb=negative_q ? -$signed(corrected_magnitude) : $signed(corrected_magnitude);

        // Select the old state during LOAD, before the divider starts. Charge
        // states do not change until COMMIT. Registering this choice keeps the
        // large state mux out of the final add/subtract and saturation path.
        update_base=0;
        update_subtract=0;
        case (stage_q)
            0: update_base=phi0_edge_q ? fric2_source_q : fric2_shape_q;
            1,3: update_base=fric2_shape_q;
            2: update_base=fric2_source_q;
            4: begin update_base=phase_q ? voice_q : fric1_q; update_subtract=1; end
            5: if (phase_q && !phi1_edge_q) update_base=f_p[0];
            6: begin update_base=phase_q ? f_y[0] : f_y[1]; update_subtract=1; end
            7: if (phase_q && !phi1_edge_q) update_base=f_p[2];
            8: begin update_base=phase_q ? f_y[2] : f_y[3]; update_subtract=1; end
            9: if (!phi1_edge_q) update_base=f_p[4];
            10: begin update_base=f_y[4]; update_subtract=1; end
            11: if (!phi1_edge_q) begin update_base=output_q; update_subtract=1; end
            default: begin end
        endcase
        // One carry chain for addition or subtraction. Saturate this 25-bit
        // result separately from the already saturated divider result.
        updated=25'(update_base_q)+(25'(result)^{25{update_subtract_q}})+25'(update_subtract_q);
    end

    // Synchronous cold reset. There is no inferred analogue warm-reset rule.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            busy<=0; event_done<=0; fault<=0; state_saturated<=0;
            phase_q<=0; phi0_edge_q<=0; phi1_edge_q<=0; route1_q<=0; route2_q<=0; output_open_q<=0;
            codes_q<=0; old_f2q_q<=0; voice_drive_q<=0; fric_drive_q<=0; old_fric_q<=0;
            voice_q<=0; fric1_q<=0; fric2_source_q<=0; fric2_shape_q<=0;
            c143_plate_q<=0; c151_plate_q<=0; c150_delta_q<=0; c151_delta_q<=0;
            output_q<=0; reconstruction<=0; phase_delta_q<=0; edge_delta_q<=0; old_voice_q<=0; old_f1_q<=0;
            stage_q<=0; step_q<=LOAD;
            for (i=0;i<4;i=i+1) begin voice_plates[i]<=0; fric1_plates[i]<=0; f2q_plates[i]<=0; filter_plates[i]<=0; end
            for (i=0;i<5;i=i+1) begin
                f_y[i]<=0; f_p[i]<=0; f_fixed[i]<=0;
                for (j=0;j<4;j=j+1) f_plates[i][j]<=0;
            end
        end else begin
            event_done<=0;
            if (event_valid && busy) fault<=1;
            if (event_valid && !busy) begin
                if (!changed) begin
                    if (output_open) reconstruction<=output_q;
                    event_done<=1;
                end else begin
                    busy<=1; step_q<=LOAD; stage_q<=0;
                    old_f2q_q<=codes_q[11:8]; old_fric_q<=fric_drive_q;
                    phase_q<=phase; phi0_edge_q<=phase_edge && !phase; phi1_edge_q<=phase_edge && phase;
                    codes_q<=codes; voice_drive_q<=voice_drive; fric_drive_q<=fric_drive;
                    route1_q<=fric1_route; route2_q<=fric2_route; output_open_q<=output_open;
                    if (!phase) begin
                        voice_q<=0; f_fixed[0]<=0; f_fixed[2]<=0; f_fixed[4]<=0;
                        for (i=0;i<4;i=i+1) begin
                            if (codes[24+i]) voice_plates[i]<=0;
                            if (codes[i]) f_plates[0][i]<=0;
                            if (codes[12+i]) f_plates[2][i]<=0;
                            if (codes[20+i]) filter_plates[i]<=f_y[4];
                        end
                    end else begin
                        fric1_q<=0; f_fixed[1]<=0; f_fixed[3]<=0;
                        if (fric1_route) c143_plate_q<=0;
                        for (i=0;i<4;i=i+1) begin
                            if (codes[28+i]) fric1_plates[i]<=0;
                            if (codes[8+i]) f2q_plates[i]<=0;
                            if (codes[4+i]) f_plates[1][i]<=0;
                            if (codes[16+i]) f_plates[3][i]<=0;
                        end
                    end
                end
            end else if (busy) begin
                case (step_q)
                    LOAD: begin
                        for (i=0;i<6;i=i+1) begin operand_q[i]<=operand[i]; coefficient_q[i]<=coefficient[i]; end
                        update_base_q<=update_base; update_subtract_q<=update_subtract;
                        denominator_q<=denominator; step_q<=MUL;
                    end
                    MUL: begin
                        for (i=0;i<6;i=i+1) product_q[i]<=operand_q[i]*coefficient_q[i];
                        step_q<=PARTIAL;
                    end
                    // Register independent pairs before the final sum. A
                    // six-product sum in one cycle creates a three-DSP48
                    // cascade that misses the 133 MHz fabric clock target.
                    PARTIAL: begin
                        for (i=0;i<3;i=i+1) partial_q[i]<=48'(product_q[i*2])+48'(product_q[i*2+1]);
                        step_q<=SUM;
                    end
                    SUM: begin numerator_q<=sum; step_q<=ROUND; end
                    ROUND: begin
                        absolute_q<=absolute_value[36:0]; negative_q<=rounded[47]; zero_q<=numerator_q==0;
                        overflow_q<=absolute_value>=({34'd0,denominator_q}<<23);
                        division_clipped_q<=rounded[47] ?
                            absolute_value>=(({34'd0,denominator_q}<<23)+{34'd0,denominator_q}) :
                            absolute_value>=({34'd0,denominator_q}<<23);
                        reciprocal_q<=reciprocal; divisor_valid_q<=divisor_valid;
                        if (!divisor_valid || (denominator_q==1 && numerator_q!=0)) fault<=1;
                        step_q<=RECIP;
                    end
                    RECIP: begin reciprocal_product_q<=absolute_q*reciprocal_q; step_q<=QPLUS; end
                    QPLUS: begin
                        quotient_q<=reciprocal_product_q[60:37];
                        correction_operand_q<={1'b0,reciprocal_product_q[60:37]}+25'd1;
                        step_q<=CORRECT;
                    end
                    CORRECT: begin correction_product_q<=correction_operand_q*denominator_q; step_q<=RESOLVE; end
                    RESOLVE: begin result<=result_comb; step_q<=COMMIT; end
                    COMMIT: begin
                        // Negative exactly -2^23 is representable; the overflow
                        // shortcut still returns it without a false clamp flag.
                        if (division_clipped_q || updated[24]!=updated[23]) state_saturated<=1;
                        step_q<=LOAD; stage_q<=stage_q+1'b1;
                        case (stage_q)
                            0: begin
                                if (phi0_edge_q) begin fric2_source_q<=sat25(updated); phase_delta_q<=result; end
                                else begin
                                    if (phi1_edge_q) fric2_shape_q<=sat25(updated);
                                    stage_q<=2;
                                end
                            end
                            1: fric2_shape_q<=sat25(updated);
                            2: begin fric2_source_q<=sat25(updated); edge_delta_q<=result; end
                            3: begin
                                fric2_shape_q<=sat25(updated);
                                c150_delta_q<=phase_q ? edge_delta_q : 24'sd0;
                                c151_delta_q<=phase_q && route2_q ? $signed(fric2_source_q)-$signed(c151_plate_q) : 25'sd0;
                                if (route2_q) c151_plate_q<=fric2_source_q;
                            end
                            4: begin
                                old_voice_q<=voice_q;
                                if (phase_q) begin
                                    voice_q<=sat25(updated);
                                    for (i=0;i<4;i=i+1) if (codes_q[24+i]) voice_plates[i]<=voice_drive_q;
                                end else begin
                                    fric1_q<=sat25(updated);
                                    for (i=0;i<4;i=i+1) if (codes_q[28+i]) fric1_plates[i]<=24'(fric_drive_q);
                                end
                            end
                            5: begin
                                if (phase_q) f_p[0]<=sat25(updated);
                                else if (phi0_edge_q || new_f2q!=0) begin
                                    f_p[1]<=result;
                                    for (i=0;i<4;i=i+1) if (codes_q[8+i]) f2q_plates[i]<=result;
                                end
                            end
                            6: begin
                                old_f1_q<=f_y[0];
                                if (phase_q) begin
                                    f_y[0]<=sat25(updated); f_fixed[0]<=f_p[0];
                                    for (i=0;i<4;i=i+1) if (codes_q[i]) f_plates[0][i]<=f_p[0];
                                end else begin
                                    f_y[1]<=sat25(updated); f_fixed[1]<=f_p[1];
                                    for (i=0;i<4;i=i+1) if (codes_q[4+i]) f_plates[1][i]<=f_p[1];
                                    if (route1_q) c143_plate_q<=fric1_q;
                                end
                            end
                            7: begin
                                if (phase_q) f_p[2]<=sat25(updated);
                                else if (phi0_edge_q) f_p[3]<=result;
                            end
                            8: begin
                                if (phase_q) begin
                                    f_y[2]<=sat25(updated); f_fixed[2]<=f_p[2];
                                    for (i=0;i<4;i=i+1) if (codes_q[12+i]) f_plates[2][i]<=f_p[2];
                                end else begin
                                    f_y[3]<=sat25(updated); f_fixed[3]<=f_p[3];
                                    for (i=0;i<4;i=i+1) if (codes_q[16+i]) f_plates[3][i]<=f_p[3];
                                    step_q<=RETIRE;
                                end
                            end
                            9: f_p[4]<=sat25(updated);
                            10: begin f_y[4]<=sat25(updated); f_fixed[4]<=f_p[4]; end
                            11: begin
                                output_q<=sat25(updated);
                                for (i=0;i<4;i=i+1) if (codes_q[20+i]) filter_plates[i]<=f_y[4];
                                step_q<=RETIRE;
                            end
                            default: begin fault<=1; step_q<=RETIRE; end
                        endcase
                    end
                    RETIRE: begin
                        if (output_open_q) reconstruction<=output_q;
                        busy<=0; event_done<=1;
                    end
                    default: begin fault<=1; busy<=0; end
                endcase
            end
        end
    end
endmodule
