`timescale 1ns / 1ps

// Native SSI-263 parameter bytes, addressed as {phone, selector}.
// Read the canonical 512-byte artifact directly. No phone translation or
// coefficient generation participates in this path.
module ssi263_parameter_rom #(
    parameter ROM_FILE = "ssi263_sc02_rom.mem"
) (
    input  logic [5:0] phone,
    input  logic [2:0] selector,
    output logic [7:0] parameter_byte
);
    (* rom_style = "distributed" *) logic [7:0] parameter_rom [0:511];

    initial $readmemh(ROM_FILE, parameter_rom);
    assign parameter_byte = parameter_rom[{phone, selector}];
endmodule
