`timescale 1ns/1ps

module trit_lane_valid #(
    parameter int TRITS = 1
) (
    input  logic [2*TRITS-1:0] lane_i,
    output logic               valid_o
);
    import trit_pkg::*;

    always_comb begin
        valid_o = 1'b1;
        for (int i = 0; i < TRITS; i++) begin
            if (!trit_pkg::trit_valid(lane_i[2*i +: 2])) begin
                valid_o = 1'b0;
            end
        end
    end
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

    logic [2*TRITS-1:0] partial_sum;
    trit_t [TRITS:0] carry;
    logic a_valid;
    logic b_valid;
    logic arithmetic_valid;

    assign carry[0] = TRIT_ZERO;

    trit_lane_valid #(.TRITS(TRITS)) valid_a (.lane_i(a_i), .valid_o(a_valid));
    trit_lane_valid #(.TRITS(TRITS)) valid_b (.lane_i(b_i), .valid_o(b_valid));

    genvar i;
    generate
        for (i = 0; i < TRITS; i++) begin : g_add
            trit_t b_eff;

            assign b_eff = SUBTRACT_B ? trit_pkg::trit_negate(b_i[2*i +: 2]) : b_i[2*i +: 2];

            trit_full_adder add_cell (
                .a(a_i[2*i +: 2]),
                .b(b_eff),
                .carry_i(carry[i]),
                .sum(partial_sum[2*i +: 2]),
                .carry_o(carry[i + 1])
            );
        end
    endgenerate

    always_comb begin
        arithmetic_valid = a_valid && b_valid && (carry[TRITS] == TRIT_ZERO);
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

    trit_lane_valid #(.TRITS(TRITS)) valid_cond (.lane_i(cond_i), .valid_o(cond_valid));

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

    always_comb begin
        valid_o = cond_valid;
        for (int j = 0; j < TRITS; j++) begin
            if (!trit_pkg::trit_valid(lane_o[2*j +: 2])) begin
                valid_o = 1'b0;
            end
        end
    end
endmodule
