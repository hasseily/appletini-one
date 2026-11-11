// hdl/apple/mockingboard.sv at 09a3bd1551dc973fb065ecdc5433cfd1ea906a0a
function automatic logic [8:0] apply_pan_gain_reference(input logic [7:0] sample,
                                              input logic [4:0] gain);
    logic [8:0] s;
    begin
        s = {1'b0, sample};
        case (gain)
            5'd0:    apply_pan_gain_reference = 9'd0;
            5'd2:    apply_pan_gain_reference = s >> 3;
            5'd4:    apply_pan_gain_reference = s >> 2;
            5'd5:    apply_pan_gain_reference = (s >> 2) + (s >> 4);
            5'd6:    apply_pan_gain_reference = (s >> 2) + (s >> 3);
            5'd7:    apply_pan_gain_reference = (s >> 2) + (s >> 3) + (s >> 4);
            5'd8:    apply_pan_gain_reference = s >> 1;
            5'd9:    apply_pan_gain_reference = (s >> 1) + (s >> 4);
            5'd10:   apply_pan_gain_reference = (s >> 1) + (s >> 3);
            5'd11:   apply_pan_gain_reference = (s >> 1) + (s >> 3) + (s >> 4);
            5'd12:   apply_pan_gain_reference = (s >> 1) + (s >> 2);
            5'd14:   apply_pan_gain_reference = (s >> 1) + (s >> 2) + (s >> 3);
            default: apply_pan_gain_reference = s;
        endcase
    end
endfunction

// hdl/apple/ssi263_stereo_mixer.sv at 09a3bd1551dc973fb065ecdc5433cfd1ea906a0a
function automatic logic [15:0] pan_coefficient_reference(input logic [15:0] gain, input logic [4:0] balance);
        case (balance)
            0:pan_coefficient_reference=0;
            2:pan_coefficient_reference=gain>>3;
            4:pan_coefficient_reference=gain>>2;
            5:pan_coefficient_reference=(gain>>2)+(gain>>4);
            6:pan_coefficient_reference=(gain>>2)+(gain>>3);
            7:pan_coefficient_reference=(gain>>2)+(gain>>3)+(gain>>4);
            8:pan_coefficient_reference=gain>>1;
            9:pan_coefficient_reference=(gain>>1)+(gain>>4);
            10:pan_coefficient_reference=(gain>>1)+(gain>>3);
            11:pan_coefficient_reference=(gain>>1)+(gain>>3)+(gain>>4);
            12:pan_coefficient_reference=(gain>>1)+(gain>>2);
            14:pan_coefficient_reference=(gain>>1)+(gain>>2)+(gain>>3);
            default:pan_coefficient_reference=gain;
        endcase
    endfunction
