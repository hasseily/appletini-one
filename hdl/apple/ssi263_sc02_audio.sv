`timescale 1ns / 1ps

// Native SSI-263A / SC-02 switched-capacitor audio model.
// Each code-controlled capacitor retains its own open-plate voltage. Events
// use the final mask atomically, sum raw pF*Q16 charge, and round once.
module ssi263_sc02_audio #(
    parameter logic [3:0] NOISE_D1_SEED = 4'h1,
    parameter logic [4:0] NOISE_D2_SEED = 5'h00,
    parameter logic [3:0] NOISE_D3_SEED = 4'h0,
    parameter logic [4:0] NOISE_D4_SEED = 5'h00,
    parameter logic [3:0] NOISE_COUNT_SEED = 4'hF,
    parameter logic signed [17:0] VOICE_TRIM_U116_STEP_Q16 = 18'sd65536
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               pd_rst_n,
    input  logic               audio_tick,
    input  logic               pw_3,
    input  logic               noise_clock_ce,
    input  logic               noise_shift_ce,
    input  logic               fric1_sw,
    input  logic               fric2_sw,
    input  logic               voice_toggle,
    input  logic               filter_phase_ce,
    input  logic               filter_phase,
    input  logic [7:0]         filter_frequency,
    input  logic [3:0]         f1_code,
    input  logic [3:0]         f2_code,
    input  logic [3:0]         f2_res_code,
    input  logic [3:0]         f3_code,
    input  logic [3:0]         f4_code,
    input  logic [3:0]         filter_amp_code,
    input  logic [3:0]         voice_amp_code,
    input  logic [3:0]         fric_amp_code,
    input  logic               closure,
    output logic signed [15:0] audio_sample
);

    localparam logic signed [17:0] FRIC_DRIVE_MAG_Q16 = 18'sd301;
    localparam integer EVENT_FIFO_DEPTH = 16;

    logic pitch_sync1_q, pitch_sync2_q, voice_load_pending_q;
    logic [3:0] voice_count_q, u60_parallel_value, voice_count_after_phi1;
    logic u60_tc, u60_tc_after_phi1;
    logic [3:0] noise_d1_q, noise_d3_q, noise_count_q;
    logic [4:0] noise_d2_q, noise_d4_q;
    logic [3:0] u75_parallel_value, noise_count_next;
    logic noise_force, noise_feedback, noise_bit;
    logic signed [17:0] fric_drive;
    logic signed [23:0] fric_drive_q16, voice_target_now;

    logic signed [23:0] voice_source_state_q;
    logic signed [23:0] voice_plate0_q, voice_plate1_q;
    logic signed [23:0] voice_plate2_q, voice_plate3_q;
    logic signed [23:0] fric1_source_state_q;
    logic signed [23:0] fric1_plate0_q, fric1_plate1_q;
    logic signed [23:0] fric1_plate2_q, fric1_plate3_q;
    logic signed [23:0] fric2_source_state_q; // U152 s
    logic signed [23:0] fric2_shape_state_q;  // U154 z
    logic signed [23:0] c143_source_plate_q, c151_source_plate_q;

    // yN output and pN first-integrator state.
    logic signed [23:0] f1_state_q, f1_history_q;
    logic signed [23:0] f2_state_q, f2_history_q;
    logic signed [23:0] f3_state_q, f3_history_q;
    logic signed [23:0] f4_state_q, f4_history_q;
    logic signed [23:0] f5_state_q, f5_history_q;

    logic signed [23:0] f1_fixed_plate_q;
    logic signed [23:0] f1_plate0_q, f1_plate1_q, f1_plate2_q, f1_plate3_q;
    logic signed [23:0] f2_res_plate0_q, f2_res_plate1_q;
    logic signed [23:0] f2_res_plate2_q, f2_res_plate3_q;
    logic signed [23:0] f2_fixed_plate_q;
    logic signed [23:0] f2_plate0_q, f2_plate1_q, f2_plate2_q, f2_plate3_q;
    logic signed [23:0] f3_fixed_plate_q;
    logic signed [23:0] f3_plate0_q, f3_plate1_q, f3_plate2_q, f3_plate3_q;
    logic signed [23:0] f4_fixed_plate_q;
    logic signed [23:0] f4_plate0_q, f4_plate1_q, f4_plate2_q, f4_plate3_q;
    logic signed [23:0] f5_fixed_plate_q;
    logic signed [23:0] filter_plate0_q, filter_plate1_q;
    logic signed [23:0] filter_plate2_q, filter_plate3_q;
    logic signed [23:0] output_hold_q, reconstruction_hold_q;

    typedef struct packed {
        logic phi0_edge, phi1_edge, phase;
        logic [3:0] f1_mask, f2_mask, f2_res_mask, f3_mask, f4_mask;
        logic [3:0] filter_mask, voice_mask, fric_mask;
        logic fric1_route, fric2_route;
        logic signed [23:0] voice_target;
        logic signed [17:0] fric_source;
    } analog_event_t;
    analog_event_t event_fifo_q [0:EVENT_FIFO_DEPTH-1];
    analog_event_t event_now_data, active_event_q;
    logic event_now_valid, event_push, engine_pop;
    logic [3:0] fifo_write_ptr_q, fifo_read_ptr_q;
    logic [4:0] fifo_count_q;

    logic observed_phase_q;
    logic [3:0] observed_f1_q, observed_f2_q, observed_f2_res_q;
    logic [3:0] observed_f3_q, observed_f4_q, observed_filter_q;
    logic [3:0] observed_voice_q, observed_fric_q;
    logic observed_fric1_route_q, observed_fric2_route_q;
    logic signed [23:0] observed_voice_target_q;
    logic signed [17:0] observed_fric_source_q;

    logic applied_phase_q;
    logic [3:0] applied_f1_q, applied_f2_q, applied_f2_res_q;
    logic [3:0] applied_f3_q, applied_f4_q, applied_filter_q;
    logic [3:0] applied_voice_q, applied_fric_q;
    logic applied_fric1_route_q, applied_fric2_route_q;
    logic signed [23:0] applied_voice_target_q;
    logic signed [17:0] applied_fric_source_q;

    logic [3:0] job_old_f1_q, job_old_f2_q, job_old_f2_res_q;
    logic [3:0] job_old_f3_q, job_old_f4_q, job_old_filter_q;
    logic [3:0] job_old_voice_q, job_old_fric_q;
    logic job_old_fric1_route_q, job_old_fric2_route_q;
    logic signed [23:0] job_old_voice_target_q;
    logic signed [17:0] job_old_fric_source_q;

    logic engine_busy_q, engine_overrun_q;
    logic [3:0] engine_stage_q;
    logic numerator_valid_q;
    logic signed [47:0] engine_div_numerator_q;
    logic [13:0] engine_div_denominator_q;
    logic signed [23:0] engine_div_result;
    logic signed [24:0] numerator_operand0;
    logic signed [24:0] numerator_operand1;
    logic signed [24:0] numerator_operand2;
    logic signed [24:0] numerator_operand3;
    logic signed [24:0] numerator_operand4;
    logic signed [24:0] numerator_operand5;
    logic signed [15:0] numerator_coefficient0;
    logic signed [15:0] numerator_coefficient1;
    logic signed [15:0] numerator_coefficient2;
    logic signed [15:0] numerator_coefficient3;
    logic signed [15:0] numerator_coefficient4;
    logic signed [15:0] numerator_coefficient5;
    logic numerator_input_valid_q;
    logic signed [24:0] numerator_operand0_q;
    logic signed [24:0] numerator_operand1_q;
    logic signed [24:0] numerator_operand2_q;
    logic signed [24:0] numerator_operand3_q;
    logic signed [24:0] numerator_operand4_q;
    logic signed [24:0] numerator_operand5_q;
    logic signed [15:0] numerator_coefficient0_q;
    logic signed [15:0] numerator_coefficient1_q;
    logic signed [15:0] numerator_coefficient2_q;
    logic signed [15:0] numerator_coefficient3_q;
    logic signed [15:0] numerator_coefficient4_q;
    logic signed [15:0] numerator_coefficient5_q;
    logic [13:0] numerator_input_denominator_q;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product0;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product1;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product2;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product3;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product4;
    (* use_dsp = "yes" *) logic signed [40:0] numerator_product5;
    logic products_valid_q;
    logic signed [40:0] numerator_product0_q;
    logic signed [40:0] numerator_product1_q;
    logic signed [40:0] numerator_product2_q;
    logic signed [40:0] numerator_product3_q;
    logic signed [40:0] numerator_product4_q;
    logic signed [40:0] numerator_product5_q;
    logic [13:0] numerator_denominator_q;
    logic signed [47:0] numerator_sum01;
    logic signed [47:0] numerator_sum23;
    logic signed [47:0] numerator_sum45;
    logic signed [47:0] numerator_sum0123;
    logic signed [47:0] numerator_sum;
    logic [13:0] numerator_denominator;
    logic signed [47:0] divider_rounded_input;
    logic [47:0] divider_absolute_input;
    logic divider_start_overflow;
    logic reciprocal_denominator_valid;
    logic [25:0] reciprocal_multiplier;
    logic reciprocal_input_valid_q;
    logic [36:0] reciprocal_operand_q;
    logic [25:0] reciprocal_multiplier_q;
    logic [13:0] reciprocal_input_denominator_q;
    logic reciprocal_input_negative_q;
    logic reciprocal_input_zero_q;
    logic reciprocal_input_overflow_q;
    logic reciprocal_input_denominator_valid_q;
    (* use_dsp = "yes" *) logic [62:0] reciprocal_product;
    logic reciprocal_product_valid_q;
    logic [62:0] reciprocal_product_q;
    logic [36:0] reciprocal_absolute_q;
    logic [13:0] reciprocal_denominator_q;
    logic reciprocal_negative_q;
    logic reciprocal_zero_q;
    logic reciprocal_overflow_q;
    logic reciprocal_denominator_valid_q;
    logic [23:0] reciprocal_q0;
    logic [24:0] correction_operand;
    logic correction_input_valid_q;
    logic [24:0] correction_operand_q;
    logic [13:0] correction_input_denominator_q;
    logic [23:0] correction_input_q0_q;
    logic [36:0] correction_input_absolute_q;
    logic correction_input_negative_q;
    logic correction_input_zero_q;
    logic correction_input_overflow_q;
    logic correction_input_denominator_valid_q;
    (* use_dsp = "yes" *) logic [38:0] correction_product;
    logic correction_valid_q;
    logic [38:0] correction_product_q;
    logic [23:0] correction_q0_q;
    logic [36:0] correction_absolute_q;
    logic correction_negative_q;
    logic correction_zero_q;
    logic correction_overflow_q;
    logic correction_denominator_valid_q;
    logic [23:0] corrected_magnitude;
    logic signed [23:0] corrected_signed_result;
    logic divider_result_valid_q;
    logic signed [23:0] divider_result_q;
    logic divider_invalid_denominator_q;
    logic engine_step_valid;
    logic signed [23:0] engine_step_result;
    logic signed [23:0] phase_s_delta_q, fric2_edge_delta_q, source_old_q;
    logic signed [23:0] f1_old_for_c127_q;
    logic signed [24:0] voice_dx_q, c127_dy_q;
    logic signed [24:0] c150_phi1_delta_q, c150_phi1_delta_hold_q;
    logic signed [24:0] c151_phi1_delta_q, c151_phi1_delta_hold_q;
    logic signed [24:0] engine_side_delta_q;
    logic signed [23:0] fric2_source_next, fric2_shape_next;
    logic [3:0] f2_keep_mask, f2_new_mask;
    logic [13:0] f2_keep_denominator;
    logic signed [24:0] c143_plate_delta;

    function automatic logic signed [47:0] sx24(input logic signed [23:0] v);
        sx24 = {{24{v[23]}}, v};
    endfunction
    function automatic logic signed [47:0] sx25(input logic signed [24:0] v);
        sx25 = {{23{v[24]}}, v};
    endfunction
    function automatic logic signed [23:0] sat24_from48(
        input logic signed [47:0] v
    );
        begin
            if (!v[47] && (|v[46:23])) sat24_from48 = 24'sh7fffff;
            else if (v[47] && !(&v[46:23])) sat24_from48 = -24'sd8388608;
            else sat24_from48 = v[23:0];
        end
    endfunction
    function automatic logic signed [23:0] add_sat24(
        input logic signed [23:0] a, input logic signed [23:0] b
    );
        add_sat24 = sat24_from48(sx24(a) + sx24(b));
    endfunction
    function automatic logic signed [23:0] sub_sat24(
        input logic signed [23:0] a, input logic signed [23:0] b
    );
        sub_sat24 = sat24_from48(sx24(a) - sx24(b));
    endfunction
    function automatic logic signed [15:0] sat16_from27(
        input logic signed [26:0] v
    );
        begin
            if (!v[26] && (|v[25:15])) sat16_from27 = 16'sh7fff;
            else if (v[26] && !(&v[25:15])) sat16_from27 = -16'sd32768;
            else sat16_from27 = v[15:0];
        end
    endfunction
    function automatic logic signed [47:0] bank_charge4(
        input logic [3:0] m, input logic signed [23:0] t,
        input logic signed [23:0] h0, input logic signed [23:0] h1,
        input logic signed [23:0] h2, input logic signed [23:0] h3,
        input integer c0, input integer c1, input integer c2, input integer c3
    );
        logic signed [24:0] d;
        logic signed [47:0] q;
        begin
            q = 0;
            if (m[0]) begin d=$signed({t[23],t})-$signed({h0[23],h0}); q=q+c0*sx25(d); end
            if (m[1]) begin d=$signed({t[23],t})-$signed({h1[23],h1}); q=q+c1*sx25(d); end
            if (m[2]) begin d=$signed({t[23],t})-$signed({h2[23],h2}); q=q+c2*sx25(d); end
            if (m[3]) begin d=$signed({t[23],t})-$signed({h3[23],h3}); q=q+c3*sx25(d); end
            bank_charge4 = q;
        end
    endfunction
    function automatic logic signed [47:0] bank_weighted4(
        input logic [3:0] m,
        input logic signed [23:0] h0, input logic signed [23:0] h1,
        input logic signed [23:0] h2, input logic signed [23:0] h3,
        input integer c0, input integer c1, input integer c2, input integer c3
    );
        logic signed [47:0] q;
        begin
            q=0;
            if(m[0])q=q+c0*sx24(h0); if(m[1])q=q+c1*sx24(h1);
            if(m[2])q=q+c2*sx24(h2); if(m[3])q=q+c3*sx24(h3);
            bank_weighted4=q;
        end
    endfunction
    function automatic logic [13:0] cap_sum4(
        input logic [3:0] m,
        input integer c0, input integer c1, input integer c2, input integer c3
    );
        integer s;
        begin
            s=0; if(m[0])s=s+c0; if(m[1])s=s+c1;
            if(m[2])s=s+c2; if(m[3])s=s+c3; cap_sum4=s[13:0];
        end
    endfunction

    // Excitation logic and final event snapshot.
    always_comb begin
        // Sheet 6: U60 P0/P1/P3 are tied to VCC and P2 is grounded.
        u60_parallel_value=4'b1011;
        voice_count_after_phi1=voice_count_q;
        if(pd_rst_n&&voice_load_pending_q) voice_count_after_phi1=u60_parallel_value;
        else if(voice_count_q!=4'hf) voice_count_after_phi1=voice_count_q+1'b1;
        u60_tc=(voice_count_q==4'hf);
        u60_tc_after_phi1=(voice_count_after_phi1==4'hf);
        // Sheet 6: U75 P0 is tied to VCC and P1/P2/P3 are grounded.
        u75_parallel_value=4'b0001;
        noise_count_next=(noise_count_q==4'hf)?u75_parallel_value:noise_count_q+1'b1;
        noise_force=~(noise_count_q[2]|noise_count_q[3]);
        noise_feedback=noise_force^noise_d1_q[3]^noise_d2_q[4]^noise_d4_q[3]^noise_d4_q[4];
        noise_bit=!(noise_d3_q[3]|(pw_3&&!voice_toggle))&&
                  (!voice_toggle||(voice_amp_code==0));
        fric_drive=noise_bit?FRIC_DRIVE_MAG_Q16:-FRIC_DRIVE_MAG_Q16;
        fric_drive_q16={{6{fric_drive[17]}},fric_drive};
        if((filter_phase_ce&&filter_phase)?u60_tc_after_phi1:u60_tc)
            voice_target_now=0;
        else voice_target_now=-$signed({{6{VOICE_TRIM_U116_STEP_Q16[17]}},VOICE_TRIM_U116_STEP_Q16});

        event_now_data.phi0_edge=filter_phase_ce&&!filter_phase;
        event_now_data.phi1_edge=filter_phase_ce&&filter_phase;
        event_now_data.phase=filter_phase;
        event_now_data.f1_mask=f1_code; event_now_data.f2_mask=f2_code;
        event_now_data.f2_res_mask=f2_res_code; event_now_data.f3_mask=f3_code;
        event_now_data.f4_mask=f4_code; event_now_data.filter_mask=filter_amp_code;
        event_now_data.voice_mask=voice_amp_code; event_now_data.fric_mask=fric_amp_code;
        event_now_data.fric1_route=fric1_sw; event_now_data.fric2_route=fric2_sw;
        event_now_data.voice_target=voice_target_now; event_now_data.fric_source=fric_drive;
        event_now_valid=filter_phase_ce||(filter_phase!=observed_phase_q)||
          (f1_code!=observed_f1_q)||(f2_code!=observed_f2_q)||
          (f2_res_code!=observed_f2_res_q)||(f3_code!=observed_f3_q)||
          (f4_code!=observed_f4_q)||(filter_amp_code!=observed_filter_q)||
          (voice_amp_code!=observed_voice_q)||(fric_amp_code!=observed_fric_q)||
          (fric1_sw!=observed_fric1_route_q)||(fric2_sw!=observed_fric2_route_q)||
          (voice_target_now!=observed_voice_target_q)||(fric_drive!=observed_fric_source_q);
        engine_pop=!engine_busy_q&&(fifo_count_q!=0);
        event_push=event_now_valid&&((fifo_count_q<EVENT_FIFO_DEPTH)||engine_pop);
    end

    // Six shared 25x16 signed lanes form every physical charge numerator.
    // The stage case selects only operands and pF coefficients.  Multipliers
    // sit after that mux, so synthesis cannot duplicate one bank per stage.
    always_comb begin
        numerator_operand0 = 25'sd0;
        numerator_operand1 = 25'sd0;
        numerator_operand2 = 25'sd0;
        numerator_operand3 = 25'sd0;
        numerator_operand4 = 25'sd0;
        numerator_operand5 = 25'sd0;
        numerator_coefficient0 = 16'sd0;
        numerator_coefficient1 = 16'sd0;
        numerator_coefficient2 = 16'sd0;
        numerator_coefficient3 = 16'sd0;
        numerator_coefficient4 = 16'sd0;
        numerator_coefficient5 = 16'sd0;
        numerator_denominator = 14'd1;
        f2_keep_mask=job_old_f2_res_q&active_event_q.f2_res_mask;
        f2_new_mask=(~job_old_f2_res_q)&active_event_q.f2_res_mask;
        f2_keep_denominator=7000+cap_sum4(f2_keep_mask,220,430,870,1800);
        c143_plate_delta=0;
        if(!active_event_q.phase&&active_event_q.fric1_route)
            c143_plate_delta=$signed({c143_source_plate_q[23],c143_source_plate_q})-
                             $signed({fric1_source_state_q[23],fric1_source_state_q});
        case(engine_stage_q)
          0: begin
            if(active_event_q.phi0_edge) begin
              numerator_operand0=$signed({fric2_shape_state_q[23],fric2_shape_state_q});
              numerator_coefficient0=16'sd3600;
            end else if(active_event_q.phi1_edge) begin
              numerator_operand0=$signed({fric2_source_state_q[23],fric2_source_state_q});
              numerator_coefficient0=-16'sd3600;
            end
            numerator_denominator=14'd3900;
          end
          1: begin
            numerator_operand0=$signed({phase_s_delta_q[23],phase_s_delta_q});
            numerator_coefficient0=-16'sd5700;
            numerator_denominator=14'd3900;
          end
          2: begin
            numerator_operand0=$signed({active_event_q.fric_source[17],
              active_event_q.fric_source})-$signed({job_old_fric_source_q[17],
              job_old_fric_source_q});
            numerator_operand1=numerator_operand0;
            numerator_operand2=numerator_operand0;
            numerator_operand3=numerator_operand0;
            numerator_coefficient0=active_event_q.fric_mask[0]?-16'sd270:16'sd0;
            numerator_coefficient1=active_event_q.fric_mask[1]?-16'sd530:16'sd0;
            numerator_coefficient2=active_event_q.fric_mask[2]?-16'sd1082:16'sd0;
            numerator_coefficient3=active_event_q.fric_mask[3]?-16'sd2160:16'sd0;
            numerator_denominator=14'd3900;
          end
          3: begin
            numerator_operand0=$signed({fric2_edge_delta_q[23],fric2_edge_delta_q});
            numerator_coefficient0=active_event_q.phase?-16'sd9300:-16'sd5700;
            numerator_denominator=14'd3900;
          end
          4: begin
            if(active_event_q.phase) begin
              numerator_operand0=$signed({active_event_q.voice_target[23],
                active_event_q.voice_target})-$signed({voice_plate0_q[23],voice_plate0_q});
              numerator_operand1=$signed({active_event_q.voice_target[23],
                active_event_q.voice_target})-$signed({voice_plate1_q[23],voice_plate1_q});
              numerator_operand2=$signed({active_event_q.voice_target[23],
                active_event_q.voice_target})-$signed({voice_plate2_q[23],voice_plate2_q});
              numerator_operand3=$signed({active_event_q.voice_target[23],
                active_event_q.voice_target})-$signed({voice_plate3_q[23],voice_plate3_q});
              numerator_coefficient0=active_event_q.voice_mask[0]?16'sd220:16'sd0;
              numerator_coefficient1=active_event_q.voice_mask[1]?16'sd430:16'sd0;
              numerator_coefficient2=active_event_q.voice_mask[2]?16'sd870:16'sd0;
              numerator_coefficient3=active_event_q.voice_mask[3]?16'sd1800:16'sd0;
              numerator_denominator=14'd3300;
            end else begin
              numerator_operand0=$signed({active_event_q.fric_source[17],
                active_event_q.fric_source})-$signed({fric1_plate0_q[23],fric1_plate0_q});
              numerator_operand1=$signed({active_event_q.fric_source[17],
                active_event_q.fric_source})-$signed({fric1_plate1_q[23],fric1_plate1_q});
              numerator_operand2=$signed({active_event_q.fric_source[17],
                active_event_q.fric_source})-$signed({fric1_plate2_q[23],fric1_plate2_q});
              numerator_operand3=$signed({active_event_q.fric_source[17],
                active_event_q.fric_source})-$signed({fric1_plate3_q[23],fric1_plate3_q});
              numerator_coefficient0=active_event_q.fric_mask[0]?16'sd270:16'sd0;
              numerator_coefficient1=active_event_q.fric_mask[1]?16'sd512:16'sd0;
              numerator_coefficient2=active_event_q.fric_mask[2]?16'sd1068:16'sd0;
              numerator_coefficient3=active_event_q.fric_mask[3]?16'sd2160:16'sd0;
              numerator_denominator=14'd3900;
            end
          end
          5: begin
            if(active_event_q.phase) begin
              if(active_event_q.phi1_edge) begin
                numerator_operand0=$signed({f1_history_q[23],f1_history_q});
                numerator_coefficient0=16'sd11500;
                numerator_operand1=$signed({f1_state_q[23],f1_state_q})-
                  $signed({voice_source_state_q[23],voice_source_state_q});
                numerator_coefficient1=16'sd2700;
                numerator_operand2=$signed({voice_source_state_q[23],voice_source_state_q});
                numerator_coefficient2=-16'sd2700;
              end else begin
                numerator_operand0=$signed({voice_source_state_q[23],voice_source_state_q})-
                  $signed({source_old_q[23],source_old_q});
                numerator_coefficient0=-16'sd5400;
              end
              numerator_denominator=14'd11700;
            end else if(active_event_q.phi0_edge) begin
              numerator_operand0=$signed({f2_history_q[23],f2_history_q});
              numerator_coefficient0=16'sd6800;
              numerator_operand1=$signed({f2_state_q[23],f2_state_q})-
                $signed({f1_state_q[23],f1_state_q});
              numerator_coefficient1=16'sd4700;
              numerator_operand2=$signed({f2_res_plate0_q[23],f2_res_plate0_q});
              numerator_operand3=$signed({f2_res_plate1_q[23],f2_res_plate1_q});
              numerator_operand4=$signed({f2_res_plate2_q[23],f2_res_plate2_q});
              numerator_operand5=$signed({f2_res_plate3_q[23],f2_res_plate3_q});
              numerator_coefficient2=active_event_q.f2_res_mask[0]?16'sd220:16'sd0;
              numerator_coefficient3=active_event_q.f2_res_mask[1]?16'sd430:16'sd0;
              numerator_coefficient4=active_event_q.f2_res_mask[2]?16'sd870:16'sd0;
              numerator_coefficient5=active_event_q.f2_res_mask[3]?16'sd1800:16'sd0;
              numerator_denominator=14'd7000+
                cap_sum4(active_event_q.f2_res_mask,220,430,870,1800);
            end else if(f2_new_mask!=0) begin
              numerator_operand0=$signed({f2_history_q[23],f2_history_q});
              numerator_coefficient0=$signed({2'b00,f2_keep_denominator});
              numerator_operand1=$signed({f2_res_plate0_q[23],f2_res_plate0_q});
              numerator_operand2=$signed({f2_res_plate1_q[23],f2_res_plate1_q});
              numerator_operand3=$signed({f2_res_plate2_q[23],f2_res_plate2_q});
              numerator_operand4=$signed({f2_res_plate3_q[23],f2_res_plate3_q});
              numerator_coefficient1=f2_new_mask[0]?16'sd220:16'sd0;
              numerator_coefficient2=f2_new_mask[1]?16'sd430:16'sd0;
              numerator_coefficient3=f2_new_mask[2]?16'sd870:16'sd0;
              numerator_coefficient4=f2_new_mask[3]?16'sd1800:16'sd0;
              numerator_denominator=f2_keep_denominator+
                cap_sum4(f2_new_mask,220,430,870,1800);
            end
          end
          6: begin
            if(active_event_q.phase) begin
              numerator_operand0=$signed({f1_history_q[23],f1_history_q})-
                $signed({f1_fixed_plate_q[23],f1_fixed_plate_q});
              numerator_operand1=$signed({f1_history_q[23],f1_history_q})-
                $signed({f1_plate0_q[23],f1_plate0_q});
              numerator_operand2=$signed({f1_history_q[23],f1_history_q})-
                $signed({f1_plate1_q[23],f1_plate1_q});
              numerator_operand3=$signed({f1_history_q[23],f1_history_q})-
                $signed({f1_plate2_q[23],f1_plate2_q});
              numerator_operand4=$signed({f1_history_q[23],f1_history_q})-
                $signed({f1_plate3_q[23],f1_plate3_q});
              numerator_coefficient0=16'sd250;
              numerator_coefficient1=active_event_q.f1_mask[0]?16'sd160:16'sd0;
              numerator_coefficient2=active_event_q.f1_mask[1]?16'sd330:16'sd0;
              numerator_coefficient3=active_event_q.f1_mask[2]?16'sd660:16'sd0;
              numerator_coefficient4=active_event_q.f1_mask[3]?16'sd1300:16'sd0;
              numerator_denominator=14'd11500;
            end else begin
              numerator_operand0=$signed({f2_history_q[23],f2_history_q})-
                $signed({f2_fixed_plate_q[23],f2_fixed_plate_q});
              numerator_operand1=$signed({f2_history_q[23],f2_history_q})-
                $signed({f2_plate0_q[23],f2_plate0_q});
              numerator_operand2=$signed({f2_history_q[23],f2_history_q})-
                $signed({f2_plate1_q[23],f2_plate1_q});
              numerator_operand3=$signed({f2_history_q[23],f2_history_q})-
                $signed({f2_plate2_q[23],f2_plate2_q});
              numerator_operand4=$signed({f2_history_q[23],f2_history_q})-
                $signed({f2_plate3_q[23],f2_plate3_q});
              numerator_operand5=c143_plate_delta;
              numerator_coefficient0=16'sd500;
              numerator_coefficient1=active_event_q.f2_mask[0]?16'sd280:16'sd0;
              numerator_coefficient2=active_event_q.f2_mask[1]?16'sd560:16'sd0;
              numerator_coefficient3=active_event_q.f2_mask[2]?16'sd1120:16'sd0;
              numerator_coefficient4=active_event_q.f2_mask[3]?16'sd2300:16'sd0;
              numerator_coefficient5=-16'sd1000;
              numerator_denominator=14'd6800;
            end
          end
          7: begin
            if(active_event_q.phase) begin
              if(active_event_q.phi1_edge) begin
                numerator_operand0=$signed({f3_history_q[23],f3_history_q});
                numerator_coefficient0=16'sd4700;
                numerator_operand1=$signed({f3_state_q[23],f3_state_q})-
                  $signed({f2_state_q[23],f2_state_q});
                numerator_coefficient1=16'sd3900;
                numerator_operand2=$signed({f1_old_for_c127_q[23],f1_old_for_c127_q})-
                  $signed({f1_state_q[23],f1_state_q});
                numerator_coefficient2=16'sd2000;
              end else begin
                numerator_operand0=$signed({f1_state_q[23],f1_state_q})-
                  $signed({f1_old_for_c127_q[23],f1_old_for_c127_q});
                numerator_coefficient0=-16'sd2000;
              end
              numerator_denominator=14'd4900;
            end else if(active_event_q.phi0_edge) begin
              numerator_operand0=$signed({f4_history_q[23],f4_history_q});
              numerator_coefficient0=16'sd4300;
              numerator_operand1=$signed({f4_state_q[23],f4_state_q})-
                $signed({f3_state_q[23],f3_state_q});
              numerator_coefficient1=16'sd4700;
              numerator_denominator=14'd4500;
            end
          end
          8: begin
            if(active_event_q.phase) begin
              numerator_operand0=$signed({f3_history_q[23],f3_history_q})-
                $signed({f3_fixed_plate_q[23],f3_fixed_plate_q});
              numerator_operand1=$signed({f3_history_q[23],f3_history_q})-
                $signed({f3_plate0_q[23],f3_plate0_q});
              numerator_operand2=$signed({f3_history_q[23],f3_history_q})-
                $signed({f3_plate1_q[23],f3_plate1_q});
              numerator_operand3=$signed({f3_history_q[23],f3_history_q})-
                $signed({f3_plate2_q[23],f3_plate2_q});
              numerator_operand4=$signed({f3_history_q[23],f3_history_q})-
                $signed({f3_plate3_q[23],f3_plate3_q});
              numerator_coefficient0=16'sd820;
              numerator_coefficient1=active_event_q.f3_mask[0]?16'sd210:16'sd0;
              numerator_coefficient2=active_event_q.f3_mask[1]?16'sd420:16'sd0;
              numerator_coefficient3=active_event_q.f3_mask[2]?16'sd820:16'sd0;
              numerator_coefficient4=active_event_q.f3_mask[3]?16'sd1640:16'sd0;
              numerator_denominator=14'd4700;
            end else begin
              numerator_operand0=$signed({f4_history_q[23],f4_history_q})-
                $signed({f4_fixed_plate_q[23],f4_fixed_plate_q});
              numerator_operand1=$signed({f4_history_q[23],f4_history_q})-
                $signed({f4_plate0_q[23],f4_plate0_q});
              numerator_operand2=$signed({f4_history_q[23],f4_history_q})-
                $signed({f4_plate1_q[23],f4_plate1_q});
              numerator_operand3=$signed({f4_history_q[23],f4_history_q})-
                $signed({f4_plate2_q[23],f4_plate2_q});
              numerator_operand4=$signed({f4_history_q[23],f4_history_q})-
                $signed({f4_plate3_q[23],f4_plate3_q});
              numerator_coefficient0=16'sd1670;
              numerator_coefficient1=active_event_q.f4_mask[0]?16'sd200:16'sd0;
              numerator_coefficient2=active_event_q.f4_mask[1]?16'sd400:16'sd0;
              numerator_coefficient3=active_event_q.f4_mask[2]?16'sd820:16'sd0;
              numerator_coefficient4=active_event_q.f4_mask[3]?16'sd1620:16'sd0;
              numerator_denominator=14'd4300;
            end
          end
          9: if(active_event_q.phase) begin
            if(active_event_q.phi1_edge) begin
              numerator_operand0=$signed({f5_history_q[23],f5_history_q});
              numerator_coefficient0=16'sd3450;
              numerator_operand1=$signed({f5_state_q[23],f5_state_q})-
                $signed({f4_state_q[23],f4_state_q});
              numerator_coefficient1=16'sd4700;
              numerator_operand2=c150_phi1_delta_hold_q;
              numerator_coefficient2=-16'sd1150;
              numerator_operand3=c151_phi1_delta_hold_q;
              numerator_coefficient3=-16'sd3700;
            end else begin
              numerator_operand0=c150_phi1_delta_hold_q;
              numerator_coefficient0=-16'sd1150;
              numerator_operand1=c151_phi1_delta_hold_q;
              numerator_coefficient1=-16'sd3700;
            end
            numerator_denominator=14'd3730;
          end
          10: if(active_event_q.phase) begin
            numerator_operand0=$signed({f5_history_q[23],f5_history_q})-
              $signed({f5_fixed_plate_q[23],f5_fixed_plate_q});
            numerator_coefficient0=16'sd4700;
            numerator_denominator=14'd3450;
          end
          11: if(active_event_q.phase) begin
            if(active_event_q.phi1_edge) begin
              numerator_operand0=$signed({output_hold_q[23],output_hold_q});
              numerator_coefficient0=16'sd2700;
            end
            numerator_operand1=$signed({f5_state_q[23],f5_state_q})-
              $signed({filter_plate0_q[23],filter_plate0_q});
            numerator_operand2=$signed({f5_state_q[23],f5_state_q})-
              $signed({filter_plate1_q[23],filter_plate1_q});
            numerator_operand3=$signed({f5_state_q[23],f5_state_q})-
              $signed({filter_plate2_q[23],filter_plate2_q});
            numerator_operand4=$signed({f5_state_q[23],f5_state_q})-
              $signed({filter_plate3_q[23],filter_plate3_q});
            numerator_coefficient1=active_event_q.filter_mask[0]?
              (active_event_q.phi1_edge?-16'sd76:16'sd76):16'sd0;
            numerator_coefficient2=active_event_q.filter_mask[1]?
              (active_event_q.phi1_edge?-16'sd150:16'sd150):16'sd0;
            numerator_coefficient3=active_event_q.filter_mask[2]?
              (active_event_q.phi1_edge?-16'sd300:16'sd300):16'sd0;
            numerator_coefficient4=active_event_q.filter_mask[3]?
              (active_event_q.phi1_edge?-16'sd600:16'sd600):16'sd0;
            numerator_denominator=14'd2750;
          end
          default: begin end
        endcase

        // Register all selected plate deltas and capacitances before the six
        // shared DSP lanes.  A code event therefore stays atomic, while the
        // selector/subtractor path ends before each multiplier input.
        numerator_product0 = numerator_operand0_q * numerator_coefficient0_q;
        numerator_product1 = numerator_operand1_q * numerator_coefficient1_q;
        numerator_product2 = numerator_operand2_q * numerator_coefficient2_q;
        numerator_product3 = numerator_operand3_q * numerator_coefficient3_q;
        numerator_product4 = numerator_operand4_q * numerator_coefficient4_q;
        numerator_product5 = numerator_operand5_q * numerator_coefficient5_q;
        numerator_sum01 =
            {{7{numerator_product0_q[40]}},numerator_product0_q} +
            {{7{numerator_product1_q[40]}},numerator_product1_q};
        numerator_sum23 =
            {{7{numerator_product2_q[40]}},numerator_product2_q} +
            {{7{numerator_product3_q[40]}},numerator_product3_q};
        numerator_sum45 =
            {{7{numerator_product4_q[40]}},numerator_product4_q} +
            {{7{numerator_product5_q[40]}},numerator_product5_q};
        numerator_sum0123 = numerator_sum01 + numerator_sum23;
        numerator_sum = numerator_sum0123 + numerator_sum45;
    end

    // Exact finite-denominator reciprocal divider.
    //
    // Let K=37 and M=floor(2^K/d).  Write 2^K=d*M+r, 0<=r<d.
    // For every non-saturated input n<d*2^23 and every live d<2^14:
    //
    //   n*M/2^K = n/d - n*r/(d*2^K)
    //   0 <= n*r/(d*2^K) < n/2^K < d/2^14 < 1.
    //
    // Thus q0=floor(n*M/2^K) is exactly floor(n/d) or one less.  The
    // registered (q0+1)*d product selects between those only two candidates.
    // This is an exact divider for the enumerated schematic capacitances,
    // not a reciprocal approximation. Signed nearest-away rounding happens
    // first by adding or subtracting d/2.
    always_comb begin
        divider_rounded_input = (engine_div_numerator_q < 0) ?
            engine_div_numerator_q - (engine_div_denominator_q >> 1) :
            engine_div_numerator_q + (engine_div_denominator_q >> 1);
        divider_absolute_input = divider_rounded_input[47] ?
            (~divider_rounded_input + 48'd1) : divider_rounded_input;
        divider_start_overflow =
            divider_absolute_input >=
            ({34'd0, engine_div_denominator_q} << 23);

        reciprocal_denominator_valid = 1'b1;
        case (engine_div_denominator_q)
            14'd2750:  reciprocal_multiplier = 26'h2FA99C9;
            14'd3300:  reciprocal_multiplier = 26'h27B8027;
            14'd3450:  reciprocal_multiplier = 26'h25FDEC1;
            14'd3730:  reciprocal_multiplier = 26'h2323D38;
            14'd3900:  reciprocal_multiplier = 26'h219BB35;
            14'd4300:  reciprocal_multiplier = 26'h1E7B5B3;
            14'd4500:  reciprocal_multiplier = 26'h1D208A5;
            14'd4700:  reciprocal_multiplier = 26'h1BE33DA;
            14'd4900:  reciprocal_multiplier = 26'h1ABFD7E;
            14'd6800:  reciprocal_multiplier = 26'h134679A;
            14'd7000:  reciprocal_multiplier = 26'h12B97D8;
            14'd7220:  reciprocal_multiplier = 26'h12276DA;
            14'd7430:  reciprocal_multiplier = 26'h11A4130;
            14'd7650:  reciprocal_multiplier = 26'h1122334;
            14'd7870:  reciprocal_multiplier = 26'h10A7965;
            14'd8090:  reciprocal_multiplier = 26'h1033A49;
            14'd8300:  reciprocal_multiplier = 26'h0FCAB3E;
            14'd8520:  reciprocal_multiplier = 26'h0F62504;
            14'd8800:  reciprocal_multiplier = 26'h0EE500E;
            14'd9020:  reciprocal_multiplier = 26'h0E8800E;
            14'd9230:  reciprocal_multiplier = 26'h0E335DC;
            14'd9450:  reciprocal_multiplier = 26'h0DDEBBC;
            14'd9670:  reciprocal_multiplier = 26'h0D8DF39;
            14'd9890:  reciprocal_multiplier = 26'h0D40C37;
            14'd10100: reciprocal_multiplier = 26'h0CFA389;
            14'd10320: reciprocal_multiplier = 26'h0CB3660;
            14'd11500: reciprocal_multiplier = 26'h0B65C6D;
            14'd11700: reciprocal_multiplier = 26'h0B33E67;
            default: begin
                reciprocal_multiplier = 26'd0;
                reciprocal_denominator_valid = 1'b0;
            end
        endcase

        // The rounded magnitude and lookup result are registered before this
        // multiply.  This keeps the 48-bit add/absolute carry chain out of
        // the cascaded reciprocal DSP path without changing either value.
        reciprocal_product = reciprocal_operand_q * reciprocal_multiplier_q;
        // For a valid, non-overflow input q0<2^23.  Bits 62:61 are therefore
        // zero; keeping bits 60:37 makes the correction lane exactly 25x14.
        reciprocal_q0 = reciprocal_product_q[60:37];
        correction_operand = {1'b0, reciprocal_q0} + 25'd1;
        // Register q0+1 before the correction DSP so the reciprocal DSP
        // cascade and incrementer do not feed another multiply in one cycle.
        correction_product = correction_operand_q *
            correction_input_denominator_q;

        corrected_magnitude = correction_q0_q;
        if (correction_product_q <=
            {{2{1'b0}}, correction_absolute_q})
            corrected_magnitude = correction_q0_q + 24'd1;

        if (correction_zero_q)
            corrected_signed_result = 24'sd0;
        else if (correction_overflow_q)
            corrected_signed_result = correction_negative_q ?
                -24'sd8388608 : 24'sh7fffff;
        else if (!correction_denominator_valid_q)
            corrected_signed_result = 24'sd0;
        else if (correction_negative_q)
            corrected_signed_result = -$signed(corrected_magnitude);
        else
            corrected_signed_result = $signed(corrected_magnitude);

        engine_step_valid = divider_result_valid_q;
        engine_step_result = divider_result_q;
        engine_div_result = engine_step_result;
    end

    // Apply a completed divider step to the two U152/U154 charge states.
    // Keep this outside the numerator selector above: the next-state result
    // depends on the divider output, while the divider input depends only on
    // registered charge state. That one-way split also matches the clocked
    // engine order and avoids a combinational scheduling cycle in simulation.
    always_comb begin
        fric2_source_next=fric2_source_state_q;
        fric2_shape_next=fric2_shape_state_q;
        if(engine_busy_q && engine_step_valid) begin
          if((engine_stage_q==0&&active_event_q.phi0_edge)||engine_stage_q==2)
            fric2_source_next=add_sat24(fric2_source_state_q,engine_step_result);
          if((engine_stage_q==0&&active_event_q.phi1_edge)||engine_stage_q==1||engine_stage_q==3)
            fric2_shape_next=add_sat24(fric2_shape_state_q,engine_step_result);
        end
    end

    always_ff @(posedge clk) begin
      if(!rstn) begin
        pitch_sync1_q<=0; pitch_sync2_q<=0; voice_load_pending_q<=0;
        voice_count_q<=4'hf; noise_d1_q<=NOISE_D1_SEED;
        noise_d2_q<=NOISE_D2_SEED; noise_d3_q<=NOISE_D3_SEED;
        noise_d4_q<=NOISE_D4_SEED; noise_count_q<=NOISE_COUNT_SEED;
        voice_source_state_q<=0; voice_plate0_q<=0; voice_plate1_q<=0;
        voice_plate2_q<=0; voice_plate3_q<=0;
        fric1_source_state_q<=0; fric1_plate0_q<=0; fric1_plate1_q<=0;
        fric1_plate2_q<=0; fric1_plate3_q<=0;
        fric2_source_state_q<=0; fric2_shape_state_q<=0;
        c143_source_plate_q<=0; c151_source_plate_q<=0;
        f1_state_q<=0; f1_history_q<=0; f2_state_q<=0; f2_history_q<=0;
        f3_state_q<=0; f3_history_q<=0; f4_state_q<=0; f4_history_q<=0;
        f5_state_q<=0; f5_history_q<=0;
        f1_fixed_plate_q<=0; f1_plate0_q<=0; f1_plate1_q<=0;
        f1_plate2_q<=0; f1_plate3_q<=0;
        f2_res_plate0_q<=0; f2_res_plate1_q<=0;
        f2_res_plate2_q<=0; f2_res_plate3_q<=0;
        f2_fixed_plate_q<=0; f2_plate0_q<=0; f2_plate1_q<=0;
        f2_plate2_q<=0; f2_plate3_q<=0;
        f3_fixed_plate_q<=0; f3_plate0_q<=0; f3_plate1_q<=0;
        f3_plate2_q<=0; f3_plate3_q<=0;
        f4_fixed_plate_q<=0; f4_plate0_q<=0; f4_plate1_q<=0;
        f4_plate2_q<=0; f4_plate3_q<=0; f5_fixed_plate_q<=0;
        filter_plate0_q<=0; filter_plate1_q<=0;
        filter_plate2_q<=0; filter_plate3_q<=0;
        output_hold_q<=0; reconstruction_hold_q<=0;
        fifo_write_ptr_q<=0; fifo_read_ptr_q<=0; fifo_count_q<=0;
        observed_phase_q<=0; observed_f1_q<=0; observed_f2_q<=0;
        observed_f2_res_q<=0; observed_f3_q<=0; observed_f4_q<=0;
        observed_filter_q<=0; observed_voice_q<=0; observed_fric_q<=0;
        observed_fric1_route_q<=0; observed_fric2_route_q<=0;
        observed_voice_target_q<=0; observed_fric_source_q<=-FRIC_DRIVE_MAG_Q16;
        applied_phase_q<=0; applied_f1_q<=0; applied_f2_q<=0;
        applied_f2_res_q<=0; applied_f3_q<=0; applied_f4_q<=0;
        applied_filter_q<=0; applied_voice_q<=0; applied_fric_q<=0;
        applied_fric1_route_q<=0; applied_fric2_route_q<=0;
        applied_voice_target_q<=0; applied_fric_source_q<=-FRIC_DRIVE_MAG_Q16;
        job_old_f1_q<=0; job_old_f2_q<=0; job_old_f2_res_q<=0;
        job_old_f3_q<=0; job_old_f4_q<=0; job_old_filter_q<=0;
        job_old_voice_q<=0; job_old_fric_q<=0;
        job_old_fric1_route_q<=0; job_old_fric2_route_q<=0;
        job_old_voice_target_q<=0; job_old_fric_source_q<=-FRIC_DRIVE_MAG_Q16;
        engine_busy_q<=0; engine_overrun_q<=0; engine_stage_q<=0;
        numerator_input_valid_q<=0;
        numerator_operand0_q<=0; numerator_operand1_q<=0;
        numerator_operand2_q<=0; numerator_operand3_q<=0;
        numerator_operand4_q<=0; numerator_operand5_q<=0;
        numerator_coefficient0_q<=0; numerator_coefficient1_q<=0;
        numerator_coefficient2_q<=0; numerator_coefficient3_q<=0;
        numerator_coefficient4_q<=0; numerator_coefficient5_q<=0;
        numerator_input_denominator_q<=1;
        products_valid_q<=0; numerator_product0_q<=0;
        numerator_product1_q<=0; numerator_product2_q<=0;
        numerator_product3_q<=0; numerator_product4_q<=0;
        numerator_product5_q<=0; numerator_denominator_q<=1;
        numerator_valid_q<=0; engine_div_numerator_q<=0;
        engine_div_denominator_q<=1;
        reciprocal_input_valid_q<=0; reciprocal_operand_q<=0;
        reciprocal_multiplier_q<=0; reciprocal_input_denominator_q<=1;
        reciprocal_input_negative_q<=0; reciprocal_input_zero_q<=0;
        reciprocal_input_overflow_q<=0;
        reciprocal_input_denominator_valid_q<=0;
        reciprocal_product_valid_q<=0; reciprocal_product_q<=0;
        reciprocal_absolute_q<=0; reciprocal_denominator_q<=1;
        reciprocal_negative_q<=0; reciprocal_zero_q<=0;
        reciprocal_overflow_q<=0; reciprocal_denominator_valid_q<=0;
        correction_input_valid_q<=0; correction_operand_q<=0;
        correction_input_denominator_q<=1; correction_input_q0_q<=0;
        correction_input_absolute_q<=0; correction_input_negative_q<=0;
        correction_input_zero_q<=0; correction_input_overflow_q<=0;
        correction_input_denominator_valid_q<=0;
        correction_valid_q<=0; correction_product_q<=0;
        correction_q0_q<=0; correction_absolute_q<=0;
        correction_negative_q<=0; correction_zero_q<=0;
        correction_overflow_q<=0; correction_denominator_valid_q<=0;
        divider_result_valid_q<=0; divider_result_q<=0;
        divider_invalid_denominator_q<=0;
        phase_s_delta_q<=0; fric2_edge_delta_q<=0; source_old_q<=0;
        f1_old_for_c127_q<=0; voice_dx_q<=0; c127_dy_q<=0;
        c150_phi1_delta_q<=0; c150_phi1_delta_hold_q<=0;
        c151_phi1_delta_q<=0; c151_phi1_delta_hold_q<=0;
        engine_side_delta_q<=0; audio_sample<=0;
      end else begin
        // Exact U61/U60 and U75/HCC4006 clock edges.
        if(!pd_rst_n) begin
          pitch_sync1_q<=0; pitch_sync2_q<=0; voice_load_pending_q<=0;
        end else if(filter_phase_ce&&!filter_phase) begin
          pitch_sync1_q<=voice_toggle; pitch_sync2_q<=pitch_sync1_q;
          if(voice_toggle&&!pitch_sync1_q) voice_load_pending_q<=1;
        end
        if(filter_phase_ce&&filter_phase) begin
          voice_count_q<=voice_count_after_phi1;
          if(pd_rst_n&&voice_load_pending_q) voice_load_pending_q<=0;
        end
        if(noise_clock_ce) noise_count_q<=noise_count_next;
        if(noise_shift_ce) begin
          noise_d1_q<={noise_d1_q[2:0],noise_d3_q[3]};
          noise_d2_q<={noise_d2_q[3:0],noise_d4_q[4]};
          noise_d3_q<={noise_d3_q[2:0],noise_d2_q[4]};
          noise_d4_q<={noise_d4_q[3:0],noise_feedback};
        end

        // FIFO capture and ordered dequeue.
        if(event_push) begin
          event_fifo_q[fifo_write_ptr_q]<=event_now_data;
          fifo_write_ptr_q<=fifo_write_ptr_q+1'b1;
          observed_phase_q<=event_now_data.phase;
          observed_f1_q<=event_now_data.f1_mask; observed_f2_q<=event_now_data.f2_mask;
          observed_f2_res_q<=event_now_data.f2_res_mask;
          observed_f3_q<=event_now_data.f3_mask; observed_f4_q<=event_now_data.f4_mask;
          observed_filter_q<=event_now_data.filter_mask;
          observed_voice_q<=event_now_data.voice_mask; observed_fric_q<=event_now_data.fric_mask;
          observed_fric1_route_q<=event_now_data.fric1_route;
          observed_fric2_route_q<=event_now_data.fric2_route;
          observed_voice_target_q<=event_now_data.voice_target;
          observed_fric_source_q<=event_now_data.fric_source;
        end else if(event_now_valid) engine_overrun_q<=1;

        if(engine_pop) begin
          active_event_q<=event_fifo_q[fifo_read_ptr_q];
          fifo_read_ptr_q<=fifo_read_ptr_q+1'b1;
          engine_busy_q<=1; engine_stage_q<=0;
          numerator_input_valid_q<=0;
          products_valid_q<=0;
          numerator_valid_q<=0;
          reciprocal_input_valid_q<=0;
          reciprocal_product_valid_q<=0;
          correction_input_valid_q<=0;
          correction_valid_q<=0;
          divider_result_valid_q<=0;
          job_old_f1_q<=applied_f1_q; job_old_f2_q<=applied_f2_q;
          job_old_f2_res_q<=applied_f2_res_q; job_old_f3_q<=applied_f3_q;
          job_old_f4_q<=applied_f4_q; job_old_filter_q<=applied_filter_q;
          job_old_voice_q<=applied_voice_q; job_old_fric_q<=applied_fric_q;
          job_old_fric1_route_q<=applied_fric1_route_q;
          job_old_fric2_route_q<=applied_fric2_route_q;
          job_old_voice_target_q<=applied_voice_target_q;
          job_old_fric_source_q<=applied_fric_source_q;
          applied_phase_q<=event_fifo_q[fifo_read_ptr_q].phase;
          applied_f1_q<=event_fifo_q[fifo_read_ptr_q].f1_mask;
          applied_f2_q<=event_fifo_q[fifo_read_ptr_q].f2_mask;
          applied_f2_res_q<=event_fifo_q[fifo_read_ptr_q].f2_res_mask;
          applied_f3_q<=event_fifo_q[fifo_read_ptr_q].f3_mask;
          applied_f4_q<=event_fifo_q[fifo_read_ptr_q].f4_mask;
          applied_filter_q<=event_fifo_q[fifo_read_ptr_q].filter_mask;
          applied_voice_q<=event_fifo_q[fifo_read_ptr_q].voice_mask;
          applied_fric_q<=event_fifo_q[fifo_read_ptr_q].fric_mask;
          applied_fric1_route_q<=event_fifo_q[fifo_read_ptr_q].fric1_route;
          applied_fric2_route_q<=event_fifo_q[fifo_read_ptr_q].fric2_route;
          applied_voice_target_q<=event_fifo_q[fifo_read_ptr_q].voice_target;
          applied_fric_source_q<=event_fifo_q[fifo_read_ptr_q].fric_source;
        end
        case({event_push,engine_pop})
          2'b10:fifo_count_q<=fifo_count_q+1'b1;
          2'b01:fifo_count_q<=fifo_count_q-1'b1;
          default:begin end
        endcase

        if(engine_busy_q) begin
          if((engine_stage_q>=4'd12) && !numerator_input_valid_q &&
             !products_valid_q &&
             !numerator_valid_q && !reciprocal_input_valid_q &&
             !reciprocal_product_valid_q && !correction_input_valid_q &&
             !correction_valid_q && !divider_result_valid_q) begin
            // Stage 11 has already committed U146.  Retire without sending
            // an artificial zero numerator through the registered pipeline.
            engine_busy_q<=0;
            engine_stage_q<=0;
          end else if(!numerator_input_valid_q && !products_valid_q &&
                      !numerator_valid_q &&
                      !reciprocal_input_valid_q &&
                      !reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            numerator_operand0_q<=numerator_operand0;
            numerator_operand1_q<=numerator_operand1;
            numerator_operand2_q<=numerator_operand2;
            numerator_operand3_q<=numerator_operand3;
            numerator_operand4_q<=numerator_operand4;
            numerator_operand5_q<=numerator_operand5;
            numerator_coefficient0_q<=numerator_coefficient0;
            numerator_coefficient1_q<=numerator_coefficient1;
            numerator_coefficient2_q<=numerator_coefficient2;
            numerator_coefficient3_q<=numerator_coefficient3;
            numerator_coefficient4_q<=numerator_coefficient4;
            numerator_coefficient5_q<=numerator_coefficient5;
            numerator_input_denominator_q<=numerator_denominator;
            numerator_input_valid_q<=1;
          end else if(numerator_input_valid_q && !products_valid_q &&
                      !numerator_valid_q && !reciprocal_input_valid_q &&
                      !reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            numerator_product0_q<=numerator_product0;
            numerator_product1_q<=numerator_product1;
            numerator_product2_q<=numerator_product2;
            numerator_product3_q<=numerator_product3;
            numerator_product4_q<=numerator_product4;
            numerator_product5_q<=numerator_product5;
            numerator_denominator_q<=numerator_input_denominator_q;
            numerator_input_valid_q<=0;
            products_valid_q<=1;
          end else if(!numerator_input_valid_q && products_valid_q &&
                      !numerator_valid_q &&
                      !reciprocal_input_valid_q &&
                      !reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            engine_div_numerator_q<=numerator_sum;
            engine_div_denominator_q<=numerator_denominator_q;
            products_valid_q<=0;
            numerator_valid_q<=1;
          end else if(!numerator_input_valid_q && numerator_valid_q &&
                      !reciprocal_input_valid_q &&
                      !reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            reciprocal_operand_q<=divider_absolute_input[36:0];
            reciprocal_multiplier_q<=reciprocal_multiplier;
            reciprocal_input_denominator_q<=engine_div_denominator_q;
            reciprocal_input_negative_q<=divider_rounded_input[47];
            reciprocal_input_zero_q<=(engine_div_numerator_q==0);
            reciprocal_input_overflow_q<=divider_start_overflow;
            reciprocal_input_denominator_valid_q<=reciprocal_denominator_valid;
            reciprocal_input_valid_q<=1;
            numerator_valid_q<=0;
            // Never turn a bad capacitance total into a plausible quotient.
            // Zero and saturated paths do not use the reciprocal result.
            if(!reciprocal_denominator_valid &&
               (engine_div_numerator_q!=0) && !divider_start_overflow)
              divider_invalid_denominator_q<=1;
          end else if(!numerator_input_valid_q &&
                      reciprocal_input_valid_q &&
                      !reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            reciprocal_product_q<=reciprocal_product;
            reciprocal_absolute_q<=reciprocal_operand_q;
            reciprocal_denominator_q<=reciprocal_input_denominator_q;
            reciprocal_negative_q<=reciprocal_input_negative_q;
            reciprocal_zero_q<=reciprocal_input_zero_q;
            reciprocal_overflow_q<=reciprocal_input_overflow_q;
            reciprocal_denominator_valid_q<=
              reciprocal_input_denominator_valid_q;
            reciprocal_input_valid_q<=0;
            reciprocal_product_valid_q<=1;
          end else if(!numerator_input_valid_q &&
                      reciprocal_product_valid_q &&
                      !correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            correction_operand_q<=correction_operand;
            correction_input_denominator_q<=reciprocal_denominator_q;
            correction_input_q0_q<=reciprocal_q0;
            correction_input_absolute_q<=reciprocal_absolute_q;
            correction_input_negative_q<=reciprocal_negative_q;
            correction_input_zero_q<=reciprocal_zero_q;
            correction_input_overflow_q<=reciprocal_overflow_q;
            correction_input_denominator_valid_q<=
              reciprocal_denominator_valid_q;
            reciprocal_product_valid_q<=0;
            correction_input_valid_q<=1;
          end else if(!numerator_input_valid_q &&
                      correction_input_valid_q && !correction_valid_q &&
                      !divider_result_valid_q) begin
            correction_product_q<=correction_product;
            correction_q0_q<=correction_input_q0_q;
            correction_absolute_q<=correction_input_absolute_q;
            correction_negative_q<=correction_input_negative_q;
            correction_zero_q<=correction_input_zero_q;
            correction_overflow_q<=correction_input_overflow_q;
            correction_denominator_valid_q<=
              correction_input_denominator_valid_q;
            correction_input_valid_q<=0;
            correction_valid_q<=1;
          end else if(!numerator_input_valid_q && correction_valid_q &&
                      !divider_result_valid_q) begin
            divider_result_q<=corrected_signed_result;
            correction_valid_q<=0;
            divider_result_valid_q<=1;
          end

          if(engine_step_valid) begin
            divider_result_valid_q<=0;
            case(engine_stage_q)
          0: begin
            // Continuously closed reset/precharge paths use the final mask.
            if(!active_event_q.phase) begin
              voice_source_state_q<=0;
              if(active_event_q.voice_mask[0])voice_plate0_q<=0;
              if(active_event_q.voice_mask[1])voice_plate1_q<=0;
              if(active_event_q.voice_mask[2])voice_plate2_q<=0;
              if(active_event_q.voice_mask[3])voice_plate3_q<=0;
              f1_fixed_plate_q<=0;
              if(active_event_q.f1_mask[0])f1_plate0_q<=0;
              if(active_event_q.f1_mask[1])f1_plate1_q<=0;
              if(active_event_q.f1_mask[2])f1_plate2_q<=0;
              if(active_event_q.f1_mask[3])f1_plate3_q<=0;
              f3_fixed_plate_q<=0;
              if(active_event_q.f3_mask[0])f3_plate0_q<=0;
              if(active_event_q.f3_mask[1])f3_plate1_q<=0;
              if(active_event_q.f3_mask[2])f3_plate2_q<=0;
              if(active_event_q.f3_mask[3])f3_plate3_q<=0;
              f5_fixed_plate_q<=0;
              if(active_event_q.filter_mask[0])filter_plate0_q<=f5_state_q;
              if(active_event_q.filter_mask[1])filter_plate1_q<=f5_state_q;
              if(active_event_q.filter_mask[2])filter_plate2_q<=f5_state_q;
              if(active_event_q.filter_mask[3])filter_plate3_q<=f5_state_q;
            end else begin
              fric1_source_state_q<=0;
              if(active_event_q.fric_mask[0])fric1_plate0_q<=0;
              if(active_event_q.fric_mask[1])fric1_plate1_q<=0;
              if(active_event_q.fric_mask[2])fric1_plate2_q<=0;
              if(active_event_q.fric_mask[3])fric1_plate3_q<=0;
              f2_fixed_plate_q<=0; f4_fixed_plate_q<=0;
              if(active_event_q.f2_res_mask[0])f2_res_plate0_q<=0;
              if(active_event_q.f2_res_mask[1])f2_res_plate1_q<=0;
              if(active_event_q.f2_res_mask[2])f2_res_plate2_q<=0;
              if(active_event_q.f2_res_mask[3])f2_res_plate3_q<=0;
              if(active_event_q.f2_mask[0])f2_plate0_q<=0;
              if(active_event_q.f2_mask[1])f2_plate1_q<=0;
              if(active_event_q.f2_mask[2])f2_plate2_q<=0;
              if(active_event_q.f2_mask[3])f2_plate3_q<=0;
              if(active_event_q.f4_mask[0])f4_plate0_q<=0;
              if(active_event_q.f4_mask[1])f4_plate1_q<=0;
              if(active_event_q.f4_mask[2])f4_plate2_q<=0;
              if(active_event_q.f4_mask[3])f4_plate3_q<=0;
              if(active_event_q.fric1_route)c143_source_plate_q<=0;
            end
            phase_s_delta_q<=0;
            if(active_event_q.phi0_edge) begin
              fric2_source_state_q<=fric2_source_next;
              phase_s_delta_q<=engine_div_result; engine_stage_q<=1;
            end else begin
              if(active_event_q.phi1_edge)fric2_shape_state_q<=fric2_shape_next;
              engine_stage_q<=2;
            end
          end
          1: begin fric2_shape_state_q<=fric2_shape_next; engine_stage_q<=2; end
          2: begin fric2_source_state_q<=fric2_source_next;
                   fric2_edge_delta_q<=engine_div_result; engine_stage_q<=3; end
          3: begin
            fric2_shape_state_q<=fric2_shape_next;
            // C150 sees U152's Phi1 source step e, not U154's stage-3
            // correction.  Phi0 U152 changes terminate at grounded FRIC_2.
            c150_phi1_delta_q<=active_event_q.phase?
              $signed({fric2_edge_delta_q[23],fric2_edge_delta_q}):0;
            c150_phi1_delta_hold_q<=active_event_q.phase?
              $signed({fric2_edge_delta_q[23],fric2_edge_delta_q}):0;
            c151_phi1_delta_q<=0; c151_phi1_delta_hold_q<=0;
            if(active_event_q.phase&&active_event_q.fric2_route) begin
              c151_phi1_delta_q<=$signed({fric2_source_state_q[23],fric2_source_state_q})-
                                      $signed({c151_source_plate_q[23],c151_source_plate_q});
              c151_phi1_delta_hold_q<=$signed({fric2_source_state_q[23],fric2_source_state_q})-
                                           $signed({c151_source_plate_q[23],c151_source_plate_q});
              c151_source_plate_q<=fric2_source_state_q;
            end else if(!active_event_q.phase&&active_event_q.fric2_route)
              c151_source_plate_q<=fric2_source_state_q;
            engine_stage_q<=4;
          end
          4: begin
            if(active_event_q.phase) begin
              source_old_q<=voice_source_state_q;
              voice_source_state_q<=sub_sat24(voice_source_state_q,engine_div_result);
              if(active_event_q.voice_mask[0])voice_plate0_q<=active_event_q.voice_target;
              if(active_event_q.voice_mask[1])voice_plate1_q<=active_event_q.voice_target;
              if(active_event_q.voice_mask[2])voice_plate2_q<=active_event_q.voice_target;
              if(active_event_q.voice_mask[3])voice_plate3_q<=active_event_q.voice_target;
            end else begin
              source_old_q<=fric1_source_state_q;
              fric1_source_state_q<=sub_sat24(fric1_source_state_q,engine_div_result);
              if(active_event_q.fric_mask[0])fric1_plate0_q<={{6{active_event_q.fric_source[17]}},active_event_q.fric_source};
              if(active_event_q.fric_mask[1])fric1_plate1_q<={{6{active_event_q.fric_source[17]}},active_event_q.fric_source};
              if(active_event_q.fric_mask[2])fric1_plate2_q<={{6{active_event_q.fric_source[17]}},active_event_q.fric_source};
              if(active_event_q.fric_mask[3])fric1_plate3_q<={{6{active_event_q.fric_source[17]}},active_event_q.fric_source};
            end
            engine_stage_q<=5;
          end
          5: begin
            if(active_event_q.phase) begin
              voice_dx_q<=$signed({voice_source_state_q[23],voice_source_state_q})-
                           $signed({source_old_q[23],source_old_q});
              engine_side_delta_q<=active_event_q.phi1_edge?
                -$signed({voice_source_state_q[23],voice_source_state_q}):
                -($signed({voice_source_state_q[23],voice_source_state_q})-
                  $signed({source_old_q[23],source_old_q}));
              if(active_event_q.phi1_edge)f1_history_q<=engine_div_result;
              else f1_history_q<=add_sat24(f1_history_q,engine_div_result);
            end else if(active_event_q.phi0_edge||(f2_new_mask!=0)) begin
              f2_history_q<=engine_div_result;
              if(active_event_q.f2_res_mask[0])f2_res_plate0_q<=engine_div_result;
              if(active_event_q.f2_res_mask[1])f2_res_plate1_q<=engine_div_result;
              if(active_event_q.f2_res_mask[2])f2_res_plate2_q<=engine_div_result;
              if(active_event_q.f2_res_mask[3])f2_res_plate3_q<=engine_div_result;
            end
            engine_stage_q<=6;
          end
          6: begin
            if(active_event_q.phase) begin
              f1_old_for_c127_q<=f1_state_q;
              f1_state_q<=sub_sat24(f1_state_q,engine_div_result);
              f1_fixed_plate_q<=f1_history_q;
              if(active_event_q.f1_mask[0])f1_plate0_q<=f1_history_q;
              if(active_event_q.f1_mask[1])f1_plate1_q<=f1_history_q;
              if(active_event_q.f1_mask[2])f1_plate2_q<=f1_history_q;
              if(active_event_q.f1_mask[3])f1_plate3_q<=f1_history_q;
            end else begin
              f2_state_q<=sub_sat24(f2_state_q,engine_div_result);
              f2_fixed_plate_q<=f2_history_q;
              if(active_event_q.f2_mask[0])f2_plate0_q<=f2_history_q;
              if(active_event_q.f2_mask[1])f2_plate1_q<=f2_history_q;
              if(active_event_q.f2_mask[2])f2_plate2_q<=f2_history_q;
              if(active_event_q.f2_mask[3])f2_plate3_q<=f2_history_q;
              if(active_event_q.fric1_route)c143_source_plate_q<=fric1_source_state_q;
            end
            engine_stage_q<=7;
          end
          7: begin
            if(active_event_q.phase) begin
              c127_dy_q<=$signed({f1_state_q[23],f1_state_q})-
                         $signed({f1_old_for_c127_q[23],f1_old_for_c127_q});
              if(active_event_q.phi1_edge)f3_history_q<=engine_div_result;
              else f3_history_q<=add_sat24(f3_history_q,engine_div_result);
            end else if(active_event_q.phi0_edge) f4_history_q<=engine_div_result;
            engine_stage_q<=8;
          end
          8: begin
            if(active_event_q.phase) begin
              f3_state_q<=sub_sat24(f3_state_q,engine_div_result);
              f3_fixed_plate_q<=f3_history_q;
              if(active_event_q.f3_mask[0])f3_plate0_q<=f3_history_q;
              if(active_event_q.f3_mask[1])f3_plate1_q<=f3_history_q;
              if(active_event_q.f3_mask[2])f3_plate2_q<=f3_history_q;
              if(active_event_q.f3_mask[3])f3_plate3_q<=f3_history_q;
            end else begin
              f4_state_q<=sub_sat24(f4_state_q,engine_div_result);
              f4_fixed_plate_q<=f4_history_q;
              if(active_event_q.f4_mask[0])f4_plate0_q<=f4_history_q;
              if(active_event_q.f4_mask[1])f4_plate1_q<=f4_history_q;
              if(active_event_q.f4_mask[2])f4_plate2_q<=f4_history_q;
              if(active_event_q.f4_mask[3])f4_plate3_q<=f4_history_q;
            end
            engine_stage_q<=9;
          end
          9: begin
            if(active_event_q.phase) begin
              if(active_event_q.phi1_edge)f5_history_q<=engine_div_result;
              else f5_history_q<=add_sat24(f5_history_q,engine_div_result);
            end
            engine_stage_q<=10;
          end
          10: begin
            if(active_event_q.phase) begin
              f5_state_q<=sub_sat24(f5_state_q,engine_div_result);
              f5_fixed_plate_q<=f5_history_q;
            end
            engine_stage_q<=11;
          end
          11: begin
            if(active_event_q.phase) begin
              if(active_event_q.phi1_edge)output_hold_q<=engine_div_result;
              else output_hold_q<=sub_sat24(output_hold_q,engine_div_result);
              if(active_event_q.filter_mask[0])filter_plate0_q<=f5_state_q;
              if(active_event_q.filter_mask[1])filter_plate1_q<=f5_state_q;
              if(active_event_q.filter_mask[2])filter_plate2_q<=f5_state_q;
              if(active_event_q.filter_mask[3])filter_plate3_q<=f5_state_q;
            end
            engine_stage_q<=12;
          end
          default: begin engine_busy_q<=0; engine_stage_q<=0; end
            endcase
          end
        end

        if(closure)reconstruction_hold_q<=output_hold_q;
        if(audio_tick)audio_sample<=sat16_from27(
          $signed({{3{reconstruction_hold_q[23]}},reconstruction_hold_q})>>>1);
      end
    end

    logic _unused_filter_frequency;
    always_comb _unused_filter_frequency=^filter_frequency;

endmodule
