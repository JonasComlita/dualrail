`timescale 1ns/1ps

module trit_lane_valid #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] lane_i,
    output logic               valid_o
);
    import trit_pkg::*;

    logic [TRITS-1:0] trit_valid_bits;

    generate
        for (genvar i = 0; i < TRITS; i++) begin : g_valid
            assign trit_valid_bits[i] = trit_pkg::trit_valid(lane_i[2*i +: 2]);
        end
    endgenerate

    assign valid_o = &trit_valid_bits;
endmodule

module trit_lane_neg #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] lane_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    logic lane_valid;

    trit_lane_valid #(.TRITS(TRITS)) valid_lane (
        .lane_i(lane_i),
        .valid_o(lane_valid)
    );

    always_comb begin
        valid_o = lane_valid;
        for (int i = 0; i < TRITS; i++) begin
            lane_o[2*i +: 2] = lane_valid ? trit_pkg::trit_negate(lane_i[2*i +: 2]) : TRIT_INVALID;
        end
    end
endmodule

module trit_lane_and #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] a_i,
    input  logic [2*TRITS-1:0] b_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    logic a_valid;
    logic b_valid;

    trit_lane_valid #(.TRITS(TRITS)) valid_a (.lane_i(a_i), .valid_o(a_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_b (.lane_i(b_i), .valid_o(b_valid));

    always_comb begin
        valid_o = a_valid && b_valid;
        for (int i = 0; i < TRITS; i++) begin
            lane_o[2*i +: 2] = valid_o ? trit_pkg::trit_min(a_i[2*i +: 2], b_i[2*i +: 2]) : TRIT_INVALID;
        end
    end
endmodule

module trit_lane_or #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] a_i,
    input  logic [2*TRITS-1:0] b_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    logic a_valid;
    logic b_valid;

    trit_lane_valid #(.TRITS(TRITS)) valid_a (.lane_i(a_i), .valid_o(a_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_b (.lane_i(b_i), .valid_o(b_valid));

    always_comb begin
        valid_o = a_valid && b_valid;
        for (int i = 0; i < TRITS; i++) begin
            lane_o[2*i +: 2] = valid_o ? trit_pkg::trit_max(a_i[2*i +: 2], b_i[2*i +: 2]) : TRIT_INVALID;
        end
    end
endmodule

module trit_lane_xsum #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] a_i,
    input  logic [2*TRITS-1:0] b_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    logic a_valid;
    logic b_valid;

    trit_lane_valid #(.TRITS(TRITS)) valid_a (.lane_i(a_i), .valid_o(a_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_b (.lane_i(b_i), .valid_o(b_valid));

    always_comb begin
        valid_o = a_valid && b_valid;
        for (int i = 0; i < TRITS; i++) begin
            lane_o[2*i +: 2] = valid_o ? trit_pkg::trit_xsum(a_i[2*i +: 2], b_i[2*i +: 2]) : TRIT_INVALID;
        end
    end
endmodule

module trit_lane_add #(
    parameter int TRITS = 1,
    parameter bit SUBTRACT_B = 1'b0
) (
    input  logic [2*TRITS-1:0] a_i,
    input  logic [2*TRITS-1:0] b_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    localparam int PREFIX_LEVELS = (TRITS > 1) ? $clog2(TRITS) : 0;

    logic [2*TRITS-1:0] b_effective;
    logic [2*TRITS-1:0] partial_sum;
    trit_t carry [TRITS + 1];
    trit_t carry_transfer [PREFIX_LEVELS + 1][TRITS][3] /* verilator split_var */;
    logic a_valid;
    logic b_valid;
    logic arithmetic_valid;
    logic carry_clear;

    assign carry[0] = TRIT_ZERO;
    assign carry_clear = carry[TRITS] == TRIT_ZERO;

    trit_lane_valid #(.TRITS(TRITS)) valid_a (.lane_i(a_i), .valid_o(a_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_b (.lane_i(b_i), .valid_o(b_valid));

    generate
        for (genvar i = 0; i < TRITS; i++) begin : g_transfer_base
            assign b_effective[2*i +: 2] = SUBTRACT_B
                ? trit_pkg::trit_negate(b_i[2*i +: 2])
                : b_i[2*i +: 2];

            assign carry_transfer[0][i][0] = trit_pkg::trit_add_carry(
                a_i[2*i +: 2], b_effective[2*i +: 2], TRIT_NEG
            );
            assign carry_transfer[0][i][1] = trit_pkg::trit_add_carry(
                a_i[2*i +: 2], b_effective[2*i +: 2], TRIT_ZERO
            );
            assign carry_transfer[0][i][2] = trit_pkg::trit_add_carry(
                a_i[2*i +: 2], b_effective[2*i +: 2], TRIT_POS
            );
        end

        // Stage zero describes each digit's carry transition for all three
        // possible inputs. Every later stage combines adjacent power-of-two
        // spans, making the carry path logarithmic in TRITS.
        for (genvar level = 0; level < PREFIX_LEVELS; level++) begin : g_prefix_level
            localparam int DISTANCE = 1 << level;

            for (genvar i = 0; i < TRITS; i++) begin : g_prefix_digit
                if (i < DISTANCE) begin : g_copy
                    for (genvar state = 0; state < 3; state++) begin : g_state
                        assign carry_transfer[level + 1][i][state] =
                            carry_transfer[level][i][state];
                    end
                end else begin : g_compose
                    for (genvar state = 0; state < 3; state++) begin : g_state
                        assign carry_transfer[level + 1][i][state] =
                            trit_pkg::trit_transfer_apply(
                                carry_transfer[level][i - DISTANCE][state],
                                carry_transfer[level][i][0],
                                carry_transfer[level][i][1],
                                carry_transfer[level][i][2]
                            );
                    end
                end
            end
        end

        for (genvar i = 0; i < TRITS; i++) begin : g_sum
            assign carry[i + 1] = trit_pkg::trit_transfer_apply(
                TRIT_ZERO,
                carry_transfer[PREFIX_LEVELS][i][0],
                carry_transfer[PREFIX_LEVELS][i][1],
                carry_transfer[PREFIX_LEVELS][i][2]
            );
            assign partial_sum[2*i +: 2] = trit_pkg::trit_add_sum(
                a_i[2*i +: 2], b_effective[2*i +: 2], carry[i]
            );
        end
    endgenerate

    always_comb begin
        arithmetic_valid = a_valid && b_valid && carry_clear;
        valid_o = arithmetic_valid;
        for (int j = 0; j < TRITS; j++) begin
            lane_o[2*j +: 2] = arithmetic_valid ? partial_sum[2*j +: 2] : TRIT_INVALID;
        end
    end
endmodule

module trit_lane_tsel #(
    parameter int TRITS = 1,
    parameter bit POISON_UNUSED_ARMS = 1'b0
) (
    input  logic [2*TRITS-1:0] cond_i,
    input  logic [2*TRITS-1:0] neg_i,
    input  logic [2*TRITS-1:0] zero_i,
    input  logic [2*TRITS-1:0] pos_i,
    output logic [2*TRITS-1:0] lane_o,
    output logic               valid_o
);
    import trit_pkg::*;

    logic cond_valid;
    logic selected_valid;

    trit_lane_valid #(.TRITS(TRITS)) valid_cond (.lane_i(cond_i), .valid_o(cond_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_selected (.lane_i(lane_o), .valid_o(selected_valid));

    genvar i;
    generate
        for (i = 0; i < TRITS; i++) begin : g_tsel
            trit_t selected;

            trit_tsel #(.POISON_UNUSED_ARMS(POISON_UNUSED_ARMS)) mux_cell (
                .cond(cond_i[2*i +: 2]),
                .neg_i(neg_i[2*i +: 2]),
                .zero_i(zero_i[2*i +: 2]),
                .pos_i(pos_i[2*i +: 2]),
                .y(selected)
            );

            assign lane_o[2*i +: 2] = cond_valid ? selected : TRIT_INVALID;
        end
    endgenerate

    assign valid_o = cond_valid && selected_valid;
endmodule
