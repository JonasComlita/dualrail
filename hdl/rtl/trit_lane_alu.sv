`timescale 1ns/1ps

module trit_lane_alu #(
    parameter int TRITS = 1
) (
    input  trit_pkg::trit_lane_op_t op_i,
    input  logic [2*TRITS-1:0]      src_a_i,
    input  logic [2*TRITS-1:0]      src_b_i,
    input  logic [2*TRITS-1:0]      src_c_i,
    input  logic [2*TRITS-1:0]      src_d_i,
    output logic [2*TRITS-1:0]      lane_o,
    output logic                    valid_o,
    output logic                    fault_o
);
    import trit_pkg::*;

    logic [2*TRITS-1:0] neg_lane;
    logic [2*TRITS-1:0] and_lane;
    logic [2*TRITS-1:0] or_lane;
    logic [2*TRITS-1:0] xsum_lane;
    logic [2*TRITS-1:0] add_lane;
    logic [2*TRITS-1:0] sub_lane;
    logic [2*TRITS-1:0] tsel_lane;
    logic neg_valid;
    logic and_valid;
    logic or_valid;
    logic xsum_valid;
    logic add_valid;
    logic sub_valid;
    logic tsel_valid;

    function automatic logic [2*TRITS-1:0] invalid_lane();
        logic [2*TRITS-1:0] out;

        for (int i = 0; i < TRITS; i++) begin
            out[2*i +: 2] = TRIT_INVALID;
        end
        return out;
    endfunction

    trit_lane_neg #(.TRITS(TRITS)) neg_unit (
        .lane_i(src_a_i),
        .lane_o(neg_lane),
        .valid_o(neg_valid)
    );

    trit_lane_and #(.TRITS(TRITS)) and_unit (
        .a_i(src_a_i),
        .b_i(src_b_i),
        .lane_o(and_lane),
        .valid_o(and_valid)
    );

    trit_lane_or #(.TRITS(TRITS)) or_unit (
        .a_i(src_a_i),
        .b_i(src_b_i),
        .lane_o(or_lane),
        .valid_o(or_valid)
    );

    trit_lane_xsum #(.TRITS(TRITS)) xsum_unit (
        .a_i(src_a_i),
        .b_i(src_b_i),
        .lane_o(xsum_lane),
        .valid_o(xsum_valid)
    );

    trit_lane_add #(.TRITS(TRITS)) add_unit (
        .a_i(src_a_i),
        .b_i(src_b_i),
        .lane_o(add_lane),
        .valid_o(add_valid)
    );

    trit_lane_add #(.TRITS(TRITS), .SUBTRACT_B(1'b1)) sub_unit (
        .a_i(src_a_i),
        .b_i(src_b_i),
        .lane_o(sub_lane),
        .valid_o(sub_valid)
    );

    trit_lane_tsel #(.TRITS(TRITS)) tsel_unit (
        .cond_i(src_a_i),
        .neg_i(src_b_i),
        .zero_i(src_c_i),
        .pos_i(src_d_i),
        .lane_o(tsel_lane),
        .valid_o(tsel_valid)
    );

    always_comb begin
        lane_o  = invalid_lane();
        valid_o = 1'b0;

        case (op_i)
            TRIT_LANE_OP_NEG: begin
                lane_o  = neg_lane;
                valid_o = neg_valid;
            end
            TRIT_LANE_OP_AND: begin
                lane_o  = and_lane;
                valid_o = and_valid;
            end
            TRIT_LANE_OP_OR: begin
                lane_o  = or_lane;
                valid_o = or_valid;
            end
            TRIT_LANE_OP_XSUM: begin
                lane_o  = xsum_lane;
                valid_o = xsum_valid;
            end
            TRIT_LANE_OP_ADD: begin
                lane_o  = add_lane;
                valid_o = add_valid;
            end
            TRIT_LANE_OP_SUB: begin
                lane_o  = sub_lane;
                valid_o = sub_valid;
            end
            TRIT_LANE_OP_TSEL: begin
                lane_o  = tsel_lane;
                valid_o = tsel_valid;
            end
            default: begin
                lane_o  = invalid_lane();
                valid_o = 1'b0;
            end
        endcase

        fault_o = !valid_o;
    end
endmodule
