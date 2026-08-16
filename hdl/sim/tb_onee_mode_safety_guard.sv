`timescale 1ns / 1ps

module tb_onee_mode_safety_guard;

    localparam integer QUIET_CYCLES = 4;
    localparam logic [11:0] OPEN_BUS_VECTOR_A = 12'b101001011010;
    localparam logic [11:0] OPEN_BUS_VECTOR_B = ~OPEN_BUS_VECTOR_A;

    logic clk = 1'b0;
    logic resetn = 1'b0;
    logic manual_enable_request = 1'b0;
    logic apple_power_present_raw = 1'b0;
    logic apple_phi0_raw = 1'b0;
    logic apple_7m_raw = 1'b0;
    logic apple_q3_raw = 1'b0;
    logic apple_m2sel_raw = 1'b0;
    logic apple_m2b0_raw = 1'b0;
    logic apple_devsel_n_raw = 1'b1;
    logic apple_reset_n_raw = 1'b1;
    logic apple_inh_n_raw = 1'b1;
    logic apple_irq_n_raw = 1'b1;
    logic apple_nmi_n_raw = 1'b1;
    logic apple_rdy_n_raw = 1'b1;
    logic apple_dma_n_raw = 1'b1;
    logic periodic_clocks = 1'b0;

    wire onee_enable_effective;
    wire force_outputs_off;
    wire physical_bus_isolate;
    wire physical_isolation_hold;
    wire apple_activity_now;
    wire apple_activity_lockout;
    wire reselect_armed;
    wire onee_selected;
    wire apple_activity_quiet;
    wire [2:0] inhibit_reason;

    onee_mode_safety_guard #(
        .QUIET_CYCLES(QUIET_CYCLES)
    ) dut (
        .clk                    (clk),
        .resetn                 (resetn),
        .manual_enable_request  (manual_enable_request),
        .apple_power_present_raw(apple_power_present_raw),
        .apple_phi0_raw         (apple_phi0_raw),
        .apple_7m_raw           (apple_7m_raw),
        .apple_q3_raw           (apple_q3_raw),
        .apple_m2sel_raw        (apple_m2sel_raw),
        .apple_m2b0_raw         (apple_m2b0_raw),
        .apple_devsel_n_raw     (apple_devsel_n_raw),
        .apple_reset_n_raw      (apple_reset_n_raw),
        .apple_inh_n_raw        (apple_inh_n_raw),
        .apple_irq_n_raw        (apple_irq_n_raw),
        .apple_nmi_n_raw        (apple_nmi_n_raw),
        .apple_rdy_n_raw        (apple_rdy_n_raw),
        .apple_dma_n_raw        (apple_dma_n_raw),
        .onee_enable_effective  (onee_enable_effective),
        .force_outputs_off      (force_outputs_off),
        .physical_bus_isolate   (physical_bus_isolate),
        .physical_isolation_hold(physical_isolation_hold),
        .apple_activity_now     (apple_activity_now),
        .apple_activity_lockout (apple_activity_lockout),
        .reselect_armed         (reselect_armed),
        .onee_selected          (onee_selected),
        .apple_activity_quiet   (apple_activity_quiet),
        .inhibit_reason         (inhibit_reason)
    );

    always #5 clk = ~clk;

    // Model the three clocks which distinguish a powered, running Apple from
    // an open connector whose inputs have settled at arbitrary levels.
    always begin
        #2;
        if (periodic_clocks) begin
            apple_phi0_raw = ~apple_phi0_raw;
            apple_7m_raw   = ~apple_7m_raw;
            apple_q3_raw   = ~apple_q3_raw;
        end
    end

    task automatic wait_clocks(input integer count);
        repeat (count)
            @(posedge clk);
        #1;
    endtask

    task automatic require_true(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $display("FAIL: %s", message);
            $fatal(1);
        end
    endtask

    task automatic require_false(input logic condition, input string message);
        if (condition !== 1'b0) begin
            $display("FAIL: %s", message);
            $fatal(1);
        end
    endtask

    task automatic set_bus_vector(input logic [11:0] value);
        {
            apple_dma_n_raw,
            apple_rdy_n_raw,
            apple_nmi_n_raw,
            apple_irq_n_raw,
            apple_reset_n_raw,
            apple_inh_n_raw,
            apple_devsel_n_raw,
            apple_m2b0_raw,
            apple_m2sel_raw,
            apple_q3_raw,
            apple_7m_raw,
            apple_phi0_raw
        } = value;
    endtask

    task automatic toggle_activity_source(input integer source_index);
        case (source_index)
            0: apple_phi0_raw     = ~apple_phi0_raw;
            1: apple_7m_raw       = ~apple_7m_raw;
            2: apple_q3_raw       = ~apple_q3_raw;
            3: apple_m2sel_raw    = ~apple_m2sel_raw;
            4: apple_m2b0_raw     = ~apple_m2b0_raw;
            5: apple_devsel_n_raw = ~apple_devsel_n_raw;
            6: apple_reset_n_raw  = ~apple_reset_n_raw;
            7: apple_inh_n_raw    = ~apple_inh_n_raw;
            8: apple_irq_n_raw    = ~apple_irq_n_raw;
            9: apple_nmi_n_raw    = ~apple_nmi_n_raw;
            10: apple_rdy_n_raw   = ~apple_rdy_n_raw;
            11: apple_dma_n_raw   = ~apple_dma_n_raw;
            default: $fatal(1, "bad activity source index");
        endcase
    endtask

    task automatic select_off_then_on;
        manual_enable_request = 1'b0;
        wait_clocks(QUIET_CYCLES + 6);
        require_false(apple_activity_lockout,
                      "off selection must clear a quiet lockout");
        require_true(reselect_armed,
                     "quiet manual off must arm a new selection");
        require_false(onee_enable_effective,
                      "manual off must keep effective enable low");
        require_false(physical_bus_isolate,
                      "quiet manual off must release physical isolation");

        manual_enable_request = 1'b1;
        #1;
        require_true(physical_bus_isolate,
                     "manual request must isolate the physical bus at once");
        wait_clocks(2);
        require_true(onee_selected,
                     "new on selection must latch after manual off");
        require_true(onee_enable_effective,
                     "quiet new selection must enable ONE//e mode");
        require_false(force_outputs_off,
                      "enabled mode must release the output kill");
        require_true(physical_bus_isolate,
                     "enabled ONE//e mode must keep the physical bus isolated");
        require_true(inhibit_reason == 3'd0,
                     "enabled mode must report no inhibit reason");
    endtask

    task automatic check_short_activity(input integer source_index);
        // Start between local clock edges and finish before the next edge. The
        // raw fail-safe and asynchronous lockout must still catch the event.
        #1;
        toggle_activity_source(source_index);
        #1;

        require_true(apple_activity_now,
                     "raw Apple transition must assert activity at once");
        require_true(force_outputs_off,
                     "raw Apple transition must assert output kill at once");
        require_false(onee_enable_effective,
                      "raw Apple transition must drop effective enable at once");
        require_true(apple_activity_lockout,
                     "raw Apple transition must set sticky lockout");
        require_true(physical_isolation_hold,
                     "activity during ONE//e must latch physical isolation");
        require_true(inhibit_reason == 3'd3,
                     "live Apple transition must report activity reason");

        toggle_activity_source(source_index);
        #1;

        wait_clocks(QUIET_CYCLES + 4);
        require_true(apple_activity_lockout,
                     "request left on must retain activity lockout");
        require_false(onee_enable_effective,
                      "request left on must not restart ONE//e mode");
        require_true(physical_bus_isolate,
                     "latched event must keep physical isolation asserted");
        require_true(inhibit_reason == 3'd4,
                     "latched event must report lockout reason");

        select_off_then_on();
    endtask

    task automatic check_aux_changes_ignored;
        integer aux_index;

        // U234 is disabled during physical isolation. Its FPGA-side outputs
        // may move as the translator turns off, so these six controls cannot
        // be ONE//e activity sources once the always-observed clocks/selects
        // have proved the Apple side quiet.
        for (aux_index = 6; aux_index < 12; aux_index = aux_index + 1)
            toggle_activity_source(aux_index);
        #1;
        require_false(apple_activity_now,
                      "isolated AUX changes must not report Apple activity");
        require_false(apple_activity_lockout,
                      "isolated AUX changes must not set activity lockout");
        require_true(onee_enable_effective,
                     "isolated AUX changes must not stop ONE//e");

        for (aux_index = 6; aux_index < 12; aux_index = aux_index + 1)
            toggle_activity_source(aux_index);
        wait_clocks(3);
        require_false(apple_activity_lockout,
                      "sampled AUX changes must remain outside the guard");
        require_true(onee_enable_effective,
                     "sampled AUX changes must leave ONE//e running");
    endtask

    task automatic check_periodic_clocks_block_arm;
        periodic_clocks = 1'b1;
        #3;

        require_true(apple_activity_lockout,
                     "continuous Apple clocks must set sticky lockout");
        require_true(force_outputs_off,
                     "continuous Apple clocks must kill ONE//e at once");
        require_false(onee_enable_effective,
                      "continuous Apple clocks must stop ONE//e");
        require_true(physical_isolation_hold,
                     "continuous Apple clocks must latch isolation");

        manual_enable_request = 1'b0;
        wait_clocks(QUIET_CYCLES + 6);
        require_false(apple_activity_quiet,
                      "continuous Apple clocks must block quiet state");
        require_false(reselect_armed,
                      "continuous Apple clocks must block reselect arm");
        require_true(apple_activity_lockout,
                     "continuous Apple clocks must retain lockout");
        require_true(physical_bus_isolate,
                     "continuous Apple clocks must retain isolation");

        periodic_clocks = 1'b0;
        wait_clocks(QUIET_CYCLES + 6);
        require_true(apple_activity_quiet,
                     "stopped stable clocks must reach quiet state");
        require_false(apple_activity_lockout,
                      "stopped clocks plus manual off must clear lockout");
        require_true(reselect_armed,
                     "stopped clocks plus manual off must arm reselect");
        require_false(physical_bus_isolate,
                      "quiet manual off must release isolation");

        select_off_then_on();
    endtask

    integer source_index;

    initial begin
        // An unplugged translator may present any stable 12-bit vector. The
        // guard must learn it after reset instead of requiring Apple idle
        // polarities which do not exist on an open connector.
        set_bus_vector(OPEN_BUS_VECTOR_A);
        #2;
        require_false(onee_enable_effective,
                      "reset must keep effective enable off");
        require_true(force_outputs_off,
                     "reset must assert the output kill");
        require_false(physical_bus_isolate,
                      "ordinary host mode must not isolate after reset");
        require_true(inhibit_reason == 3'd1,
                     "reset must report reset reason");

        #10;
        resetn = 1'b1;
        wait_clocks(QUIET_CYCLES + 6);
        require_true(apple_activity_quiet,
                     "stable open-bus vector must reach quiet state");
        require_false(apple_activity_lockout,
                      "stable open-bus vector must clear startup lockout");
        require_true(reselect_armed,
                     "stable open-bus vector must arm selection");
        select_off_then_on();

        // A reset with the request still on must kill the VM. Isolation stays
        // asserted because this is not ordinary host mode.
        resetn = 1'b0;
        #1;
        require_false(onee_enable_effective,
                      "reset must kill an enabled ONE//e mode");
        require_true(physical_bus_isolate,
                     "request high during reset must retain isolation");
        #9;
        resetn = 1'b1;
        wait_clocks(QUIET_CYCLES + 6);
        require_false(onee_enable_effective,
                      "request high across reset must not restart mode");
        select_off_then_on();

        // Both complementary stable baselines make the first pulse on every
        // always-observed pin run once in each direction. Each pulse ends
        // before a fabric edge.
        for (source_index = 0; source_index < 6;
             source_index = source_index + 1)
            check_short_activity(source_index);

        manual_enable_request = 1'b0;
        #1;
        set_bus_vector(OPEN_BUS_VECTOR_B);
        select_off_then_on();
        for (source_index = 0; source_index < 6;
             source_index = source_index + 1)
            check_short_activity(source_index);

        check_aux_changes_ignored();

        // Stable levels may become the open-bus baseline. Running Apple
        // clocks may not: their edges keep the guard locked and unarmed.
        check_periodic_clocks_block_arm();

        // Slot power is a continuous veto. Manual off cannot release the bus
        // or clear the lockout until power disappears and the quiet time runs.
        apple_power_present_raw = 1'b1;
        #1;
        require_false(onee_enable_effective,
                      "Apple slot power must kill ONE//e at once");
        require_true(physical_bus_isolate,
                     "Apple slot power during ONE//e must retain isolation");
        require_true(physical_isolation_hold,
                     "Apple slot power must latch the isolation hold");
        require_true(inhibit_reason == 3'd2,
                     "Apple slot power must report the power veto reason");

        manual_enable_request = 1'b0;
        wait_clocks(QUIET_CYCLES + 4);
        require_true(apple_activity_lockout,
                     "slot power must prevent lockout clear");
        require_true(physical_bus_isolate,
                     "slot power must prevent isolation release");

        // Reset with manual mode off is the explicit escape back to ordinary
        // host mode, even if slot power remains present.
        resetn = 1'b0;
        wait_clocks(2);
        require_false(physical_bus_isolate,
                      "reset with manual off must restore ordinary host mode");
        require_false(physical_isolation_hold,
                      "reset with manual off must clear isolation hold");
        resetn = 1'b1;
        wait_clocks(2);
        require_false(physical_bus_isolate,
                      "powered ordinary host mode must remain connected");
        require_false(onee_enable_effective,
                      "slot power must keep ONE//e disabled after reset");

        apple_power_present_raw = 1'b0;
        wait_clocks(QUIET_CYCLES + 6);
        require_false(apple_activity_lockout,
                      "quiet manual off must clear lockout after power loss");
        require_false(physical_bus_isolate,
                      "quiet manual off must reconnect the host-side bus");
        manual_enable_request = 1'b1;
        #1;
        require_true(physical_bus_isolate,
                     "new request after power loss must isolate at once");
        wait_clocks(2);
        require_true(onee_enable_effective,
                     "new off-on selection after power loss must enable mode");

        // Manual off is also a direct fail-safe path.
        manual_enable_request = 1'b0;
        #1;
        require_true(force_outputs_off,
                     "manual off must assert output kill without a clock");
        require_false(onee_enable_effective,
                      "manual off must drop effective enable without a clock");
        require_true(inhibit_reason == 3'd6,
                     "manual off must report its reason");
        wait_clocks(2);
        require_false(physical_bus_isolate,
                      "safe manual off must return to ordinary host mode");

        $display("ONEE MODE SAFETY GUARD PASS");
        $finish;
    end

endmodule
