// Shared video and framebuffer constants used by PL video modules.
// Keeps geometry and AXI burst sizing definitions in one place so timing,
// pattern generation, and framebuffer fetch logic stay aligned.

package video_pkg;

    // Active video geometry is fixed at 1080p; frame rate switches via blanking.
    localparam [11:0] VIDEO_ACTIVE_W = 12'd1920;
    localparam [11:0] VIDEO_ACTIVE_H = 12'd1080;

    // Framebuffer format and HP0 read-burst geometry. RGB565 is the
    // designed output depth: the DVI pins are 5:6:5 and fb_reader
    // streams these pixels straight onto them.
    localparam integer FB_BYTES_PER_PIXEL     = 2;
    localparam integer AXI_HP0_BURST_BEATS    = 16;
    localparam integer AXI_HP0_BEAT_BYTES     = 8;   // 64-bit HP0
    localparam integer AXI_HP0_BURST_BYTES    = AXI_HP0_BURST_BEATS * AXI_HP0_BEAT_BYTES;

endpackage
