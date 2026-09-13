`timescale 1ns / 1ps

// Select writes that the TURBO motherboard mirror must retire promptly.
// This does not select renderer records: its shadow still receives every
// possible video write, including pages that are currently hidden.
module vtw_video_policy (
    input  logic [16:0] address,
    input  logic        sw_text,
    input  logic        sw_mixed,
    input  logic        sw_page2,
    input  logic        sw_hires,
    input  logic        sw_80store,
    input  logic        sw_80col,
    input  logic        post_main_wide,
    input  logic        overlay_match,
    output logic        mirror_active
);
    wire is_aux = address[16];
    // 80STORE changes CPU banking but keeps the video scanner on page 1.
    wire page2 = sw_page2 && !sw_80store;
    wire text_page = page2 ? (address[15:10] == 6'b000010) :
                             (address[15:10] == 6'b000001);
    wire hires_page = page2 ? (address[15:13] == 3'b010) :
                              (address[15:13] == 3'b001);
    wire graphics_range = (address[15:13] >= 3'b001) &&
                          (address[15:13] <= 3'b100);

    // AUX graphics also hold SHR pixels, controls and palettes. Its state
    // is not fully described by the classic switches, so keep the whole
    // extended range immediate. Paged SHR uses the same range in MAIN.
    wire extended_graphics = (is_aux || post_main_wide) && graphics_range;

    // A2Li stamps mode/load controls in the unused MAIN page-2 holes even
    // when page 1 is displayed. Keep both holes immediate in every mode.
    wire legacy_metadata = !is_aux &&
        ((address[15:3] == (16'h0878 >> 3)) ||
         (address[15:3] == (16'h4078 >> 3)));

    assign mirror_active = overlay_match || extended_graphics ||
        legacy_metadata ||
        (text_page && (sw_text || sw_mixed || !sw_hires) &&
         (!is_aux || sw_80col)) ||
        (hires_page && !is_aux && !sw_text && sw_hires);
endmodule
