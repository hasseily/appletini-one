`timescale 1ns / 1ps

module tb_vtw_video_policy;
    logic [16:0] address = 0;
    logic sw_text = 0, sw_mixed = 0, sw_page2 = 0, sw_hires = 0;
    logic sw_80store = 0, sw_80col = 0;
    logic post_main_wide = 0, overlay_match = 0;
    logic mirror_active;
    int checks = 0;

    vtw_video_policy dut (.*);

    // An interval reference uses framebuffer base/size instead of the
    // production address-bit decode. It includes conservative extension
    // ranges independently from the classic display choice.
    function automatic bit expected(input int physical);
        int addr, text_base, hires_base;
        bit aux, active;
        addr = physical & 65535;
        aux = physical >= 65536;
        text_base = (sw_page2 && !sw_80store) ? 2048 : 1024;
        hires_base = (sw_page2 && !sw_80store) ? 16384 : 8192;
        active = overlay_match;
        if ((aux || post_main_wide) && addr >= 8192 && addr < 40960)
            active = 1;
        if (!aux && ((addr >= 2168 && addr < 2176) ||
                     (addr >= 16504 && addr < 16512)))
            active = 1;
        if (addr >= text_base && addr < text_base + 1024 &&
            (sw_text || sw_mixed || !sw_hires) && (!aux || sw_80col))
            active = 1;
        if (!aux && addr >= hires_base && addr < hires_base + 8192 &&
            !sw_text && sw_hires)
            active = 1;
        return active;
    endfunction

    task automatic check(input int physical);
        address = 17'(physical);
        #1;
        if (mirror_active !== expected(physical))
            $fatal(1, "policy mismatch addr=%05x text=%b mixed=%b page2=%b hires=%b store80=%b col80=%b wide=%b overlay=%b",
                   address, sw_text, sw_mixed, sw_page2, sw_hires,
                   sw_80store, sw_80col, post_main_wide, overlay_match);
        checks++;
    endtask

    initial begin
        // Every classic switch combination, both SHR-wide states and both
        // banks. Check every page boundary and its last byte, plus each
        // legacy metadata boundary which lies inside a 256-byte page.
        for (int mode = 0; mode < 128; mode++) begin
            {post_main_wide, sw_80col, sw_80store, sw_hires,
             sw_page2, sw_mixed, sw_text} = 7'(mode);
            for (int page = 0; page < 512; page++) begin
                check(page * 256);
                check(page * 256 + 255);
            end
            for (int bank = 0; bank < 2; bank++) begin
                for (int i = 0; i < 10; i++) begin
                    check(bank * 65536 + 'h0877 + i);
                    check(bank * 65536 + 'h4077 + i);
                end
                check(bank * 65536 + 'h08DF);
            end
        end

        // Full-screen DHGR is the reported case. Sweep both complete banks
        // to catch any accidental holes or range spill at the target mode.
        sw_text = 0;
        sw_mixed = 0;
        sw_page2 = 0;
        sw_hires = 1;
        sw_80store = 0;
        sw_80col = 1;
        post_main_wide = 0;
        for (int addr = 0; addr < 131072; addr++) check(addr);
        check('h108DF);
        if (mirror_active) $fatal(1, "fullscreen DHGR must defer AUX $08DF");

        // A later page-2 mixed-mode access must make that byte active.
        sw_mixed = 1;
        sw_page2 = 1;
        check('h108DF);
        if (!mirror_active) $fatal(1, "mixed page 2 must mirror AUX $08DF");
        sw_80store = 1;
        check('h108DF);
        if (mirror_active) $fatal(1, "80STORE must keep scanner on page 1");

        // Overlay selection overrides ordinary video ranges, in either bank.
        overlay_match = 1;
        for (int addr = 0; addr < 131072; addr++) check(addr);
        $display("VTW VIDEO POLICY PASS (%0d checks)", checks);
        $finish;
    end
endmodule
