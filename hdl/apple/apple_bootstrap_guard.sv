`timescale 1ns / 1ps

/* Fail-closed low-/M2SEL GS boot classifier.
 *
 * The monitor probes C707,C705,C703,C701 in descending order before C700.
 * Only selected low-pin reads advance or expose one byte. The following
 * selected low C700 fetch provisionally identifies a IIgs and arms normal
 * active-low /M2SEL qualification before the ROM calls internal $FE1F.
 * Every high-pin cycle remains silent while identity is unknown; physical
 * IIe cold autoboot is intentionally unsupported. */
module apple_bootstrap_guard (
    input  logic                  clk,
    input  logic                  resetn,
    input  logic                  onee_enable_effective,
    input  logic                  machine_identity_reported,
    input  logic                  machine_identity_legacy,
    input  logic                  machine_identity_iigs,
    input  logic                  reported_identity_fault,
    input  logic                  iigs_external_slot_mask_valid,
    input  logic                  iigs_slot7_allowed,
    input  globals::AppleBus_read physical_ab_read,
    output logic                  boot_menu_physical_visible,
    output logic                  bootstrap_identity_valid,
    output logic                  bootstrap_identity_legacy,
    output logic                  bootstrap_identity_iigs,
    output logic                  unknown_boot_select_seen
);

    typedef enum logic [2:0] {
        EXPECT_C707,
        EXPECT_C705,
        EXPECT_C703,
        EXPECT_C701,
        EXPECT_C700,
        CLASS_IIGS
    } bootstrap_state_t;

    bootstrap_state_t state_q;
    logic accepted_read_q;

    wire selected_cycle = physical_ab_read.serve_en &&
                          physical_ab_read.cycle_valid &&
                          !physical_ab_read.m2sel;
    wire selected_read = selected_cycle && physical_ab_read.rw;

    logic [15:0] expected_signature_addr;
    always_comb begin
        case (state_q)
            EXPECT_C707: expected_signature_addr = 16'hC707;
            EXPECT_C705: expected_signature_addr = 16'hC705;
            EXPECT_C703: expected_signature_addr = 16'hC703;
            EXPECT_C701: expected_signature_addr = 16'hC701;
            default:     expected_signature_addr = 16'hFFFF;
        endcase
    end

    wire signature_state = (state_q == EXPECT_C707) ||
                           (state_q == EXPECT_C705) ||
                           (state_q == EXPECT_C703) ||
                           (state_q == EXPECT_C701);
    wire signature_read_match = selected_read && signature_state &&
                                (physical_ab_read.addr ==
                                 expected_signature_addr);
    wire low_c700_classifier = selected_read &&
                               (state_q == EXPECT_C700) &&
                               (physical_ab_read.addr == 16'hC700);
    wire classified_cycle_allowed = selected_cycle &&
        (state_q == CLASS_IIGS) &&
        (!machine_identity_reported ||
         (machine_identity_iigs &&
          (!iigs_external_slot_mask_valid || iigs_slot7_allowed)));
    wire cycle_allowed = signature_read_match || low_c700_classifier ||
                         classified_cycle_allowed;

    /* The boot card registers its reply after serve_en. Hold the accepted
     * token until the next address boundary so the byte remains allowed
     * through the wrapper's physical data window. */
    always_ff @(posedge clk) begin
        if (!resetn || !physical_ab_read.res || onee_enable_effective ||
            reported_identity_fault) begin
            accepted_read_q <= 1'b0;
        end else if (cycle_allowed) begin
            accepted_read_q <= 1'b1;
        end else if (physical_ab_read.addr_en ||
                     physical_ab_read.serve_en) begin
            accepted_read_q <= 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!resetn || onee_enable_effective) begin
            state_q <= EXPECT_C707;
        end else if (!physical_ab_read.res &&
                     !machine_identity_reported) begin
            state_q <= EXPECT_C707;
        end else if (!machine_identity_reported &&
                     !reported_identity_fault) begin
            if (low_c700_classifier) begin
                state_q <= CLASS_IIGS;
            end else if (selected_read &&
                         (physical_ab_read.addr[15:8] == 8'hC7)) begin
                if (physical_ab_read.addr == 16'hC707) begin
                    state_q <= EXPECT_C705;
                end else if (signature_read_match) begin
                    case (state_q)
                        EXPECT_C705: state_q <= EXPECT_C703;
                        EXPECT_C703: state_q <= EXPECT_C701;
                        EXPECT_C701: state_q <= EXPECT_C700;
                        default:     state_q <= EXPECT_C707;
                    endcase
                end else begin
                    state_q <= EXPECT_C707;
                end
            end
        end
    end

    assign bootstrap_identity_legacy = 1'b0;
    assign bootstrap_identity_iigs = (state_q == CLASS_IIGS);
    assign bootstrap_identity_valid = bootstrap_identity_iigs;
    assign unknown_boot_select_seen = (state_q != EXPECT_C707);

    /* The boot card registers a read response on the same edge that this
     * token accepts the cycle. Reads may prepare a hidden reply, while the
     * token gates every state-changing data phase and every physical output.
     * This keeps the live classifier out of the card-to-card response cone. */
    assign boot_menu_physical_visible = accepted_read_q &&
                                        !onee_enable_effective &&
                                        !reported_identity_fault;

endmodule
