`timescale 1ns / 1ps
// Shared video and framebuffer constants used by PL video modules.
// Keeps geometry and AXI burst sizing definitions in one place so timing,
// pattern generation, and framebuffer fetch logic stay aligned.

package video_pkg;

    // Largest surface and reset mode. Mode IDs also form the PS register ABI.
    localparam [11:0] VIDEO_ACTIVE_W = 12'd1920;
    localparam [11:0] VIDEO_ACTIVE_H = 12'd1080;
    localparam [3:0] VIDEO_DEFAULT_MODE = 4'd4;
    localparam integer VIDEO_MODE_COUNT = 6;

    typedef struct packed {
        logic [11:0] width;
        logic [11:0] height;
        logic [11:0] h_front;
        logic [11:0] h_sync;
        logic [11:0] h_total;
        logic [11:0] v_front;
        logic [11:0] v_sync;
        logic [11:0] v_total;
        logic h_positive;
        logic v_positive;
    } video_mode_t;

    // DMT/CVT timings, about 60 Hz. 1200x800 uses reduced blanking. 1080p keeps the
    // existing Apple 50/60 Hz blanking policy in video_timing_gen.
    function automatic video_mode_t video_mode(input logic [3:0] id);
        case (id)
            0: video_mode = '{1024, 768, 24, 136, 1344, 3, 6, 806, 0, 0};
            1: video_mode = '{1200, 800, 48, 32, 1360, 3, 10, 828, 1, 0};
            2: video_mode = '{1280, 1024, 48, 112, 1688, 1, 3, 1066, 1, 1};
            3: video_mode = '{1680, 1050, 48, 32, 1840, 3, 6, 1080, 1, 0};
            5: video_mode = '{1360, 768, 64, 112, 1792, 3, 6, 795, 1, 1};
            default: video_mode = '{1920, 1080, 88, 44, 2200, 4, 5, 1125, 1, 1};
        endcase
    endfunction

    function automatic logic [17:0] video_frame_bursts(input logic [3:0] id);
        video_mode_t mode;
        logic [31:0] bytes;
        mode = video_mode(id);
        bytes = 32'(mode.width) * 32'(mode.height) * 2;
        // The last 1680x1050 burst has 32 padding pixels. Scanout consumes
        // only active pixels and resets the FIFO before the next frame.
        video_frame_bursts = 18'((bytes + 127) / 128);
    endfunction

    // PG065 AXI clock configuration registers, 150 MHz reference.
    // VCOs are 675..1080 MHz; output clocks are 65, 67.5, 108, 119,
    // 148.5 and 85.5 MHz. Fractions use thousandths. The Wizard AXI wrapper
    // derives fraction-enable bits from these fields (PG065); do not
    // confuse these input values with its internal DRP register image.
    function automatic logic [31:0] video_clock_feedback(input logic [3:0] id);
        case (id)
            0: video_clock_feedback = (13 << 8) | 2;
            1: video_clock_feedback = (9 << 8) | 2;
            2: video_clock_feedback = (36 << 8) | 5;
            3: video_clock_feedback = (750 << 16) | (29 << 8) | 5;
            5: video_clock_feedback = (57 << 8) | 10;
            default: video_clock_feedback = (750 << 16) | (24 << 8) | 5;
        endcase
    endfunction

    function automatic logic [31:0] video_clock_output(input logic [3:0] id);
        case (id)
            0: video_clock_output = 15;
            1: video_clock_output = 10;
            2: video_clock_output = 10;
            3: video_clock_output = (500 << 8) | 7;
            5: video_clock_output = 10;
            default: video_clock_output = 5;
        endcase
    endfunction

    // Framebuffer format and HP0 read-burst geometry. RGB565 is the
    // designed output depth: the DVI pins are 5:6:5 and fb_reader
    // streams these pixels straight onto them.
    localparam integer FB_BYTES_PER_PIXEL     = 2;
    localparam integer AXI_HP0_BURST_BEATS    = 16;
    localparam integer AXI_HP0_BEAT_BYTES     = 8;   // 64-bit HP0
    localparam integer AXI_HP0_BURST_BYTES    = AXI_HP0_BURST_BEATS * AXI_HP0_BEAT_BYTES;

endpackage
