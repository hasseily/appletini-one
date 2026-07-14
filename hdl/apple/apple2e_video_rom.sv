`timescale 1ns / 1ps

// Apple //e video character ROM reader (2732 image).
// Uses a 4KB ROM image and exposes one 8-bit row at a time.
//
// ROM image: apple2e_video_rom_342_0265_a.mem, enhanced Apple //e part
// 342-0265-A.
//
// Addressing:
// - glyph-major rows: {bank_sel, char_code[7:0], row_idx[2:0]}
// - `bank_sel` selects the upper/lower 2KB half of the 4KB ROM image.
//   The text renderer keeps the text path on a fixed half and
//   handles Apple //e ALTCHARSET by remapping the character-code quadrant
//   for the affected byte range ($40-$7F).
//
// The ROM bytes are active-low pixel bits for the normal Apple //e path,
// so this module inverts them and outputs active-high row bits.

module apple2e_video_rom (
    input  wire        bank_sel,
    input  wire [7:0]  char_code,
    input  wire [2:0]  row_idx,
    output reg  [7:0]  row_bits
);
    reg [7:0] rom [0:4095];
    wire [11:0] rom_addr = {bank_sel, char_code, row_idx};

    initial begin
        $readmemh("apple2e_video_rom_342_0265_a.mem", rom);
    end

    always @* begin
        row_bits = ~rom[rom_addr];
    end
endmodule
