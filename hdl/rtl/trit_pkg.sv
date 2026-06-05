package trit_pkg;
    typedef logic [1:0] trit_t;

    localparam trit_t TRIT_NEG     = 2'b00;
    localparam trit_t TRIT_ZERO    = 2'b01;
    localparam trit_t TRIT_POS     = 2'b10;
    localparam trit_t TRIT_INVALID = 2'b11;

    function automatic logic trit_valid(input trit_t t);
        return t != TRIT_INVALID;
    endfunction

    function automatic logic signed [2:0] trit_decode(input trit_t t);
        unique case (t)
            TRIT_NEG:  return -3'sd1;
            TRIT_ZERO: return  3'sd0;
            TRIT_POS:  return  3'sd1;
            default:   return  3'sd0;
        endcase
    endfunction

    function automatic trit_t trit_encode(input logic signed [2:0] value);
        unique case (value)
            -3'sd1: return TRIT_NEG;
             3'sd0: return TRIT_ZERO;
             3'sd1: return TRIT_POS;
            default: return TRIT_INVALID;
        endcase
    endfunction

    function automatic trit_t trit_negate(input trit_t t);
        unique case (t)
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
        unique case (total)
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
endpackage
