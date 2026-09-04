`timescale 1ns/1ps

package trit_pkg;
    typedef logic [1:0] trit_t;

    localparam logic [1:0] TRIT_NEG     = 2'b00;
    localparam logic [1:0] TRIT_ZERO    = 2'b01;
    localparam logic [1:0] TRIT_POS     = 2'b10;
    localparam logic [1:0] TRIT_INVALID = 2'b11;

    function automatic logic trit_valid(input trit_t t);
        return t != TRIT_INVALID;
    endfunction

    function automatic logic signed [2:0] trit_decode(input trit_t t);
        case (t)
            TRIT_NEG:  return -3'sd1;
            TRIT_ZERO: return  3'sd0;
            TRIT_POS:  return  3'sd1;
            default:   return  3'sd0;
        endcase
    endfunction

    function automatic trit_t trit_encode(input logic signed [2:0] value);
        case (value)
            -3'sd1: return TRIT_NEG;
             3'sd0: return TRIT_ZERO;
             3'sd1: return TRIT_POS;
            default: return TRIT_INVALID;
        endcase
    endfunction

    function automatic trit_t trit_negate(input trit_t t);
        case (t)
            TRIT_NEG:  return TRIT_POS;
            TRIT_ZERO: return TRIT_ZERO;
            TRIT_POS:  return TRIT_NEG;
            default:   return TRIT_INVALID;
        endcase
    endfunction

    function automatic trit_t trit_min(input trit_t a, input trit_t b);
        if (!trit_valid(a) || !trit_valid(b)) return TRIT_INVALID;
        return (a < b) ? a : b;
    endfunction

    function automatic trit_t trit_max(input trit_t a, input trit_t b);
        if (!trit_valid(a) || !trit_valid(b)) return TRIT_INVALID;
        return (a > b) ? a : b;
    endfunction

    function automatic trit_t trit_xsum(input trit_t a, input trit_t b);
        logic signed [2:0] sum;

        if (!trit_valid(a) || !trit_valid(b)) return TRIT_INVALID;

        sum = trit_decode(a) + trit_decode(b);
        if (sum > 3'sd1) begin
            sum = sum - 3'sd3;
        end else if (sum < -3'sd1) begin
            sum = sum + 3'sd3;
        end
        return trit_encode(sum);
    endfunction

    function automatic trit_t trit_xdiff(input trit_t a, input trit_t b);
        return trit_xsum(a, trit_negate(b));
    endfunction

    typedef struct packed {
        trit_t sum;
        trit_t carry;
    } trit_add_result_t;

    typedef enum logic [2:0] {
        TRIT_LANE_OP_NEG  = 3'd0,
        TRIT_LANE_OP_AND  = 3'd1,
        TRIT_LANE_OP_OR   = 3'd2,
        TRIT_LANE_OP_XSUM = 3'd3,
        TRIT_LANE_OP_ADD  = 3'd4,
        TRIT_LANE_OP_SUB  = 3'd5,
        TRIT_LANE_OP_TSEL = 3'd6
    } trit_lane_op_t;

    function automatic trit_add_result_t trit_add3(
        input trit_t a,
        input trit_t b,
        input trit_t carry_i
    );
        logic signed [2:0] total;
        trit_add_result_t result;

        if (!trit_valid(a) || !trit_valid(b) || !trit_valid(carry_i)) begin
            result.sum   = TRIT_INVALID;
            result.carry = TRIT_INVALID;
            return result;
        end

        total = trit_decode(a) + trit_decode(b) + trit_decode(carry_i);
        case (total)
            -3'sd3: begin result.sum = TRIT_ZERO; result.carry = TRIT_NEG;  end
            -3'sd2: begin result.sum = TRIT_POS;  result.carry = TRIT_NEG;  end
            -3'sd1: begin result.sum = TRIT_NEG;  result.carry = TRIT_ZERO; end
             3'sd0: begin result.sum = TRIT_ZERO; result.carry = TRIT_ZERO; end
             3'sd1: begin result.sum = TRIT_POS;  result.carry = TRIT_ZERO; end
             3'sd2: begin result.sum = TRIT_NEG;  result.carry = TRIT_POS;  end
             3'sd3: begin result.sum = TRIT_ZERO; result.carry = TRIT_POS;  end
            default: begin result.sum = TRIT_INVALID; result.carry = TRIT_INVALID; end
        endcase
        return result;
    endfunction

    function automatic trit_t trit_add_sum(
        input trit_t a,
        input trit_t b,
        input trit_t carry_i
    );
        trit_add_result_t result;

        result = trit_add3(a, b, carry_i);
        return result.sum;
    endfunction

    function automatic trit_t trit_add_carry(
        input trit_t a,
        input trit_t b,
        input trit_t carry_i
    );
        trit_add_result_t result;

        result = trit_add3(a, b, carry_i);
        return result.carry;
    endfunction

    function automatic trit_t trit_transfer_apply(
        input trit_t carry_i,
        input trit_t carry_if_neg,
        input trit_t carry_if_zero,
        input trit_t carry_if_pos
    );
        // A carry transition is a three-entry function. Composing these
        // functions is associative, which permits a parallel-prefix network.
        case (carry_i)
            TRIT_NEG:  return carry_if_neg;
            TRIT_ZERO: return carry_if_zero;
            TRIT_POS:  return carry_if_pos;
            default:   return TRIT_INVALID;
        endcase
    endfunction
endpackage
