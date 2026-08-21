`timescale 1ns / 1ps

// Pure Enhanced //e $C011-$C01F status decoder. Callers supply their own VBL
// source and low seven bits because those inputs differ between the physical
// vTW shortcut and the isolated ONE//e motherboard.
module apple_c01x_status_decode (
    input  logic [3:0]               selector,
    input  globals::SoftSwitchState  sss,
    input  logic                     vbl_bar,
    input  logic [6:0]               low_bits,
    output logic [7:0]               status_byte
);

    logic status_bit;

    always_comb begin
        unique case (selector)
            4'h1:    status_bit = sss.sw_lcram_bank2; // $C011 RDLCBNK2
            4'h2:    status_bit = sss.sw_lcram_read;  // $C012 RDLCRAM
            4'h3:    status_bit = sss.sw_ramrd;       // $C013 RDRAMRD
            4'h4:    status_bit = sss.sw_ramwrt;      // $C014 RDRAMWRT
            4'h5:    status_bit = sss.sw_intcxrom;    // $C015 RDCXROM
            4'h6:    status_bit = sss.sw_altzp;       // $C016 RDALTZP
            4'h7:    status_bit = sss.sw_slotc3rom;   // $C017 RDC3ROM
            4'h8:    status_bit = sss.sw_80store;     // $C018 RD80STORE
            4'h9:    status_bit = vbl_bar;            // $C019 RDVBLBAR
            4'hA:    status_bit = sss.sw_text;        // $C01A RDTEXT
            4'hB:    status_bit = sss.sw_mixed;       // $C01B RDMIXED
            4'hC:    status_bit = sss.sw_page2;       // $C01C RDPAGE2
            4'hD:    status_bit = sss.sw_hires;       // $C01D RDHIRES
            4'hE:    status_bit = sss.sw_altcharset;  // $C01E RDALTCHAR
            4'hF:    status_bit = sss.sw_80col;       // $C01F RD80COL
            default: status_bit = 1'b0;
        endcase
        status_byte = {status_bit, low_bits};
    end

endmodule
