`timescale 1ns/1ps
module tb_video_pixel_mask;
    logic clk=0, resetn=0;
    always #3.5 clk=~clk;
    logic [607:0] cfg=0;
    logic [15:0] pixel=0;
    logic [11:0] x=0,y=0;
    logic de=0,hs=0,vs=0;
    wire [15:0] result;
    wire ode,ohs,ovs;
    video_pixel_mask dut(clk,resetn,cfg,pixel,x,y,de,hs,vs,result,ode,ohs,ovs);
    function automatic [15:0] reference_pixel;
        input [15:0] p;
        input integer xx,yy;
        integer r,g,b,m,selected,lx,ly,n;
        bit enabled,dim_all;
        begin
            m=cfg[1:0]; lx=xx-int'(cfg[32+:12]); ly=yy-int'(cfg[64+:12]);
            enabled=xx>=cfg[32+:12] && xx<cfg[48+:12] &&
                    yy>=cfg[64+:12] && yy<cfg[80+:12];
            for(n=0;n<8;n=n+1)
                if(n<cfg[7:4] && xx>=cfg[96+n*64+:12] && xx<cfg[112+n*64+:12] &&
                   yy>=cfg[128+n*64+:12] && yy<cfg[144+n*64+:12]) enabled=0;
            r=((p>>11)&31)*8+((p>>13)&7);
            g=((p>>5)&63)*4+((p>>9)&3);
            b=(p&31)*8+((p>>2)&7);
            selected=(lx%3+(m==2 ? (ly&1):0))%3;
            dim_all=m==3 && (lx%3==2 || ly%3==2);
            if(enabled && (dim_all || ((m==1 || m==2) && selected!=0))) r-=r>>2;
            if(enabled && (dim_all || ((m==1 || m==2) && selected!=1))) g-=g>>2;
            if(enabled && (dim_all || ((m==1 || m==2) && selected!=2))) b-=b>>2;
            reference_pixel=16'(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
        end
    endfunction
    logic [18:0] expected[0:3];
    int checked=0;
    always @(posedge clk) begin
        if(!resetn) for(int k=0;k<4;k++) expected[k]<=0;
        else begin
            expected[0]<={de,hs,vs,reference_pixel(pixel,x,y)};
            for(int k=1;k<4;k++) expected[k]<=expected[k-1];
        end
        #1;
        if({ode,ohs,ovs,result} !== expected[3])
            $fatal(1,"mask pipeline mismatch got=%h expected=%h case=%0d",{ode,ohs,ovs,result},expected[3],checked);
        checked++;
    end
    initial begin
        repeat(8) @(negedge clk); resetn=1;
        cfg[32+:32]={16'd4095,16'd7}; cfg[64+:32]={16'd4095,16'd11};
        for(int mode=0;mode<4;mode++) begin
            cfg[1:0]=2'(mode);
            for(int p=0;p<65536;p++) begin
                @(negedge clk);
                pixel=16'(p); x=12'(7+p%1920); y=12'(11+(p/3)%1080);
                de=1; hs=1'(p>>3); vs=1'(p>>9);
            end
        end
        // All eight exclusions, half-open edges, nonzero origins and blanks.
        cfg[7:4]=8;
        for(int n=0;n<8;n++) begin
            cfg[96+n*64+:32]={16'(30+n*20),16'(20+n*20)};
            cfg[128+n*64+:32]={16'(30+n*5),16'(15+n*5)};
        end
        for(int p=0;p<12000;p++) begin
            @(negedge clk); pixel=16'($random); x=12'(p%210); y=12'((p/210)%65);
            cfg[1:0]=2'(p%4); de=1'(p%3); hs=1'(p>>4); vs=1'(p>>7);
        end
        repeat(8) @(negedge clk);
        $display("VIDEO PIXEL MASK PASS: %0d samples",checked); $finish;
    end
endmodule

module tb_video_mask_config;
    logic clk=0,pclk=0,pclock_enable=1;
    always #5 clk=~clk;
    always #3.5 if(pclock_enable) pclk=~pclk;
    logic resetn=0,video_resetn=0,we=0,latch=0,vblank=0,blank=1;
    logic [7:0] wa=0,ra=0;
    logic [31:0] wd=0,base=0;
    logic [3:0] strb=15;
    wire [31:0] rd;
    wire [607:0] cfg;
    video_mask_config dut(clk,resetn,we,wa,ra,wd,strb,rd,latch,base,
                          pclk,video_resetn,vblank,blank,cfg);
    task write_word(input integer address,input [31:0] data,input [3:0] strobe=15);
        @(negedge clk); wa=8'(address); wd=data; strb=strobe; we=1;
        @(negedge clk); we=0; ra=8'(address); #1;
    endtask
    task start_blank;
        @(negedge pclk); blank=1; vblank=1;
        @(negedge pclk); vblank=0;
        repeat(8) @(negedge clk);
    endtask
    task reader_latch(input [31:0] address);
        @(negedge clk); base=address; latch=1;
        @(negedge clk); latch=0;
    endtask
    task wait_transfer;
        repeat(45) @(negedge pclk);
    endtask
    initial begin
        repeat(8) @(negedge clk); resetn=1; video_resetn=1;
        for(int slot=0;slot<3;slot++) begin
            write_word(64+slot*32,32'h10000000+32'(slot*4096));
            write_word(65+slot*32,32'(slot+1));
            for(int w=2;w<20;w++) write_word(64+slot*32+w,32'(slot*1000+w));
        end
        write_word(66,32'habcd1234,3); if(rd!==32'h00001234) $fatal(1,"write strobe");
        write_word(95,32'hffffffff); if(rd!==0) $fatal(1,"reserved word");
        for(int slot=0;slot<3;slot++) begin
            start_blank(); reader_latch(32'h10000000+32'(slot*4096)); wait_transfer();
            if(cfg[1:0]!==2'(slot+1) || cfg[576+:32]!==32'(slot*1000+19))
                $fatal(1,"wrong frame bank %0d config=%h",slot,cfg);
            @(negedge pclk); blank=0;
            write_word(65+slot*32,0); wait_transfer();
            if(cfg[1:0]!==2'(slot+1)) $fatal(1,"live bank changed frame");
            write_word(65+slot*32,32'(slot+1));
        end
        start_blank(); reader_latch(32'h77770000); wait_transfer();
        if(cfg!==0) $fatal(1,"unmatched tag did not bypass");
        // Active-picture arrival must not change this or the next frame.
        start_blank(); blank=0; reader_latch(32'h10000000); wait_transfer();
        if(cfg!==0) $fatal(1,"late active packet accepted");
        start_blank(); wait_transfer(); if(cfg!==0) $fatal(1,"old packet carried");
        // Clock stop and mode reset with an outstanding mailbox packet.
        @(negedge pclk); pclock_enable=0; video_resetn=0;
        reader_latch(32'h10000000); repeat(12) @(negedge clk);
        pclock_enable=1; repeat(12) @(negedge pclk); video_resetn=1;
        start_blank(); reader_latch(32'h10001000); wait_transfer();
        if(cfg[1:0]!==2) $fatal(1,"reset frame association");
        // A global reset clears both banks and the synchronized epoch.
        @(negedge clk); resetn=0; video_resetn=0;
        repeat(8) @(negedge clk); resetn=1;
        repeat(8) @(negedge pclk); video_resetn=1;
        if(cfg!==0) $fatal(1,"global reset did not disable mask");
        write_word(64,32'h20000000); write_word(65,3);
        start_blank(); reader_latch(32'h20000000); wait_transfer();
        if(cfg[1:0]!==3) $fatal(1,"global reset recovery");
        $display("VIDEO MASK CONFIG PASS"); $finish;
    end
    initial begin #200000; $fatal(1,"config timeout"); end
endmodule
