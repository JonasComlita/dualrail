`timescale 1ns/1ps

module trit_neg (
    input  trit_pkg::trit_t a,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb y = trit_pkg::trit_negate(a);
endmodule

module trit_and (
    input  trit_pkg::trit_t a,
    input  trit_pkg::trit_t b,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb y = trit_pkg::trit_min(a, b);
endmodule

module trit_or (
    input  trit_pkg::trit_t a,
    input  trit_pkg::trit_t b,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb y = trit_pkg::trit_max(a, b);
endmodule

module trit_xsum (
    input  trit_pkg::trit_t a,
    input  trit_pkg::trit_t b,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb y = trit_pkg::trit_xsum(a, b);
endmodule

module trit_xdiff (
    input  trit_pkg::trit_t a,
    input  trit_pkg::trit_t b,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb y = trit_pkg::trit_xdiff(a, b);
endmodule

module trit_full_adder (
    input  trit_pkg::trit_t a,
    input  trit_pkg::trit_t b,
    input  trit_pkg::trit_t carry_i,
    output trit_pkg::trit_t sum,
    output trit_pkg::trit_t carry_o
);
    import trit_pkg::*;

    trit_add_result_t result;

    always_comb begin
        result  = trit_pkg::trit_add3(a, b, carry_i);
        sum     = result.sum;
        carry_o = result.carry;
    end
endmodule

module trit_tsel #(
    parameter bit POISON_UNUSED_ARMS = 1'b0
) (
    input  trit_pkg::trit_t cond,
    input  trit_pkg::trit_t neg_i,
    input  trit_pkg::trit_t zero_i,
    input  trit_pkg::trit_t pos_i,
    output trit_pkg::trit_t y
);
    import trit_pkg::*;

    always_comb begin
        if (!trit_valid(cond) ||
            (POISON_UNUSED_ARMS && (!trit_valid(neg_i) ||
                                    !trit_valid(zero_i) ||
                                    !trit_valid(pos_i)))) begin
            y = TRIT_INVALID;
        end else begin
            unique case (cond)
                TRIT_NEG:  y = neg_i;
                TRIT_ZERO: y = zero_i;
                TRIT_POS:  y = pos_i;
                default:   y = TRIT_INVALID;
            endcase
        end
    end
endmodule
