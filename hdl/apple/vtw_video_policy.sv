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
    input  logic        shr_active,
    input  logic        overlay_match,
    output logic        mirror_active,
    output logic        capture_only
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

    // Synthetic SHR has no motherboard display. This policy is consumed
    // only for writes the caller already classifies as video/overlay; it
    // does not widen that address window. While SHR is selected, classic
    // MAIN/AUX text/HGR pages are also private work memory, not physical
    // display buffers. Keep their ordered capture records without creating
    // mirror traffic. The captured C029[7:6]==11 renderer state is definitive.
    assign capture_only = shr_active;
    // Outside synthetic SHR preserve the conservative AUX/DHGR policy.
    wire extended_graphics = (is_aux || post_main_wide) && graphics_range;

    // A2Li stamps mode/load controls in the unused MAIN page-2 holes even
    // when page 1 is displayed. Keep both holes immediate in every mode.
    wire legacy_metadata = !is_aux &&
        ((address[15:3] == (16'h0878 >> 3)) ||
         (address[15:3] == (16'h4078 >> 3)));

    assign mirror_active = !capture_only && (overlay_match || extended_graphics ||
        legacy_metadata ||
        (text_page && (sw_text || sw_mixed || !sw_hires) &&
         (!is_aux || sw_80col)) ||
        (hires_page && !is_aux && !sw_text && sw_hires));
endmodule
