`timescale 1ns / 1ps
// Production engine+shadow+capture/FIFO fixture, byte-address row oracle.
module tb_vtw_copy_rows;
    tb_vtw_copy_publish #(.RUN_TESTS(1'b0)) p();
    integer cases=0, strided_clocks, separate_clocks;
    task automatic configure(input integer rows, sg, dg, input logic publish);
        p.fixture();
        p.base.copy_rows=1;
        p.base.rows_minus1=8'(rows-1);
        p.base.source_gap=8'(sg);
        p.base.destination_gap=8'(dg);
        p.base.publish=publish;
    endtask
    task automatic success(input integer src,dst,width,rows,sg,dg,input logic publish);
        integer started;
        configure(rows,sg,dg,publish);
        p.base.label=$sformatf("rows %06x->%06x width%0d rows%0d gaps%0d/%0d publish%0d",src,dst,width,rows,sg,dg,publish);
        started=p.base.clocks;
        p.base.launch(src,dst,width,0);
        p.base.wait_done();
        p.base.check(!p.base.error && !p.base.aborted && p.base.completed==width*rows,"rows completion");
        p.base.verify_memory();
        p.base.check(p.command_accepted==(publish?width*rows:0),"rows capture count");
        if(width==128 && rows==8 && src=='h800 && dst=='h12010 && publish) begin
            strided_clocks=p.base.clocks-started;
            $display("ROWS MEASURE bytes=%0d clocks=%0d reads=%0d",width*rows,strided_clocks,p.base.sh_reads);
        end
        p.drain();
        p.base.sticky();
        cases++;
    endtask
    task automatic invalid(input integer src,dst,width,rows,sg,dg,input logic publish,is_fill);
        configure(rows,sg,dg,publish);
        p.base.label="invalid row spans";
        p.base.launch(src,dst,width,is_fill);
        p.base.wait_done();
        p.base.check(p.base.error && p.base.completed==0 && !p.base.aborted,"invalid rows accepted");
        p.base.check(p.base.sh_reads+p.base.sh_writes+p.base.ps_reads+p.base.ps_writes==0 && p.command_accepted==0,"invalid rows mutated memory/capture");
        p.base.verify_memory();p.drain();cases++;
    endtask
    initial begin
        integer started,saved,budget;
        p.ab_read='0;p.sss='0;
        repeat(3)p.base.tick();p.base.rstn=1;p.base.tick();
        for(int mode=0;mode<3;mode++)
            for(int offset=0;offset<4;offset++)
                for(int width=1;width<=9;width++)
                    success((mode==1?'h20200:'h800)+offset,
                            (mode==2?'h21000:'h12010)+(3-offset),width,8,2,3,mode!=2);
        success('h800,'h12010,128,8,0,32,1);
        separate_clocks=0;
        for(int row=0;row<8;row++) begin
            p.fixture();p.base.copy_rows=0;
            started=p.base.clocks;p.base.launch('h800+128*row,'h12010+160*row,128,0);p.base.wait_done();
            separate_clocks+=p.base.clocks-started;p.base.verify_memory();p.drain();
        end
        $display("ROWS COMPARISON bytes=1024 strided_clocks=%0d separate_clocks=%0d",strided_clocks,separate_clocks);
        success('h800,'h21000,2,32,0,1,0); // Several short rows share each PSRAM line.
        success('h20200,'h12010,3,8,5,5,1);
        success('h800,'h12010,1,256,0,1,1);
        success('hBFFD,'h19CFD,3,1,255,255,1); // Final-row gaps are not applied.
        invalid('h800,'h12010,0,8,0,32,1,0);
        invalid('h800,'h12010,0,2,1,1,1,0);
        invalid('h800,'h12010,256,256,0,0,1,0); // total65536 rejected
        invalid('hBFF0,'h12010,8,2,8,0,1,0);
        invalid('h800,'h19CF0,8,2,0,8,1,0);
        invalid('h800,'h1FFF0,8,2,0,8,0,0);
        invalid('h800,'h900,4,3,128,128,0,0); // Bounding overlap, though active rows disjoint.
        invalid('h100,'h12010,8,2,0,0,1,0);
        invalid('h800,'h12010,8,2,0,0,1,1); // COPY_ROWS is not FILL_ROWS.
        // Real capture FIFO backpressure, retained word and byte lane stable.
        configure(128,0,32,1);p.consume=0;
        p.base.launch('h800,'h12010,128,0);
        budget=100000;
        while(!(p.base.publish_valid&&!p.copy_ready)&&budget>0)begin p.base.tick();budget--;end
        p.base.check(budget>0,"rows never backpressured");saved=p.base.completed;
        repeat(30)p.base.tick();p.base.check(p.base.completed==saved,"blocked rows progressed");
        p.consume=1;p.consumer_period=5;p.base.wait_done();p.base.verify_memory();p.drain();cases++;
        // Abort, hold/mode loss and synchronous reset never emit extra bytes.
        for(int cause=0;cause<4;cause++)begin
            configure(8,0,32,1);p.base.label=$sformatf("rows cancel cause%0d",cause);p.base.launch('h800,'h12010,128,0);
            while(p.base.completed<137)p.base.tick();
            saved=p.base.completed;
            if(cause==0)p.base.abort_req=1;
            if(cause==1)p.base.permit=0;
            if(cause==2)p.transport=0;
            if(cause==3)p.base.rstn=0;
            p.base.tick();
            if(cause==3)begin
                p.base.track_progress=0;p.base.check(!p.base.busy&&p.base.completed==0,"reset did not clear engine");
                // A hardware reset discards queued capture records as well.
                p.accepted=p.received;
                p.base.rstn=1;p.base.tick();
            end else begin
                p.base.wait_done();p.base.check(p.base.aborted&&p.base.completed==saved,"cancel prefix wrong");
            end
            p.base.check(p.command_accepted==saved,"cancel admitted extra byte");
            p.base.verify_memory();p.drain();cases++;
        end
        // Accepted private PSRAM write must drain after cancellation, exact prefix.
        configure(8,0,3,0);p.base.launch('h800,'h21001,5,0);
        p.base.await_request(0,1);p.base.abort_req=1;p.base.tick();
        p.base.wait_done();p.base.check(p.base.aborted&&p.base.completed>0,"accepted rows write did not drain");
        p.base.verify_memory();p.drain();cases++;
        $display("VTW COPY ROWS PASS cases=%0d checks=%0d",cases,p.base.checks);
        $finish;
    end
endmodule
