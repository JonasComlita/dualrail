`timescale 1ns/1ps

module trit_gate_tb;
    import trit_pkg::*;

    localparam int TRITS = 5;

    trit_t a;
    trit_t b;
    trit_t c;
    trit_t y_neg;
    trit_t y_and;
    trit_t y_or;
    trit_t y_xsum;
    trit_t y_xdiff;
    trit_t y_tsel;
    trit_t y_sum;
    trit_t y_carry;

    logic [2*TRITS-1:0] lane_a;
    logic [2*TRITS-1:0] lane_b;
    logic [2*TRITS-1:0] lane_cond;
    logic [2*TRITS-1:0] lane_neg;
    logic [2*TRITS-1:0] lane_zero;
    logic [2*TRITS-1:0] lane_pos;
    logic [2*TRITS-1:0] lane_y_neg;
    logic [2*TRITS-1:0] lane_y_and;
    logic [2*TRITS-1:0] lane_y_or;
    logic [2*TRITS-1:0] lane_y_xsum;
    logic [2*TRITS-1:0] lane_y_add;
    logic [2*TRITS-1:0] lane_y_sub;
    logic [2*TRITS-1:0] lane_y_tsel;
    logic lane_valid_neg;
    logic lane_valid_and;
    logic lane_valid_or;
    logic lane_valid_xsum;
    logic lane_valid_add;
    logic lane_valid_sub;
    logic lane_valid_tsel;

    int failures = 0;

    trit_neg neg_gate (.a(a), .y(y_neg));
    trit_and and_gate (.a(a), .b(b), .y(y_and));
    trit_or or_gate (.a(a), .b(b), .y(y_or));
    trit_xsum xsum_gate (.a(a), .b(b), .y(y_xsum));
    trit_xdiff xdiff_gate (.a(a), .b(b), .y(y_xdiff));
    trit_full_adder add_gate (.a(a), .b(b), .carry_i(c), .sum(y_sum), .carry_o(y_carry));
    trit_tsel tsel_gate (
        .cond(a),
        .neg_i(TRIT_NEG),
        .zero_i(TRIT_ZERO),
        .pos_i(TRIT_POS),
        .y(y_tsel)
    );

    trit_lane_neg #(.TRITS(TRITS)) lane_neg_gate (
        .lane_i(lane_a),
        .lane_o(lane_y_neg),
        .valid_o(lane_valid_neg)
    );

    trit_lane_and #(.TRITS(TRITS)) lane_and_gate (
        .a_i(lane_a),
        .b_i(lane_b),
        .lane_o(lane_y_and),
        .valid_o(lane_valid_and)
    );

    trit_lane_or #(.TRITS(TRITS)) lane_or_gate (
        .a_i(lane_a),
        .b_i(lane_b),
        .lane_o(lane_y_or),
        .valid_o(lane_valid_or)
    );

    trit_lane_xsum #(.TRITS(TRITS)) lane_xsum_gate (
        .a_i(lane_a),
        .b_i(lane_b),
        .lane_o(lane_y_xsum),
        .valid_o(lane_valid_xsum)
    );

    trit_lane_add #(.TRITS(TRITS)) lane_add_gate (
        .a_i(lane_a),
        .b_i(lane_b),
        .lane_o(lane_y_add),
        .valid_o(lane_valid_add)
    );

    trit_lane_add #(.TRITS(TRITS), .SUBTRACT_B(1'b1)) lane_sub_gate (
        .a_i(lane_a),
        .b_i(lane_b),
        .lane_o(lane_y_sub),
        .valid_o(lane_valid_sub)
    );

    trit_lane_tsel #(.TRITS(TRITS)) lane_tsel_gate (
        .cond_i(lane_cond),
        .neg_i(lane_neg),
        .zero_i(lane_zero),
        .pos_i(lane_pos),
        .lane_o(lane_y_tsel),
        .valid_o(lane_valid_tsel)
    );

    function automatic trit_t encode_model(input int value);
        unique case (value)
            -1: return TRIT_NEG;
             0: return TRIT_ZERO;
             1: return TRIT_POS;
            default: return TRIT_INVALID;
        endcase
    endfunction

    function automatic int decode_model(input trit_t value);
        unique case (value)
            TRIT_NEG: return -1;
            TRIT_ZERO: return 0;
            TRIT_POS: return 1;
            default: return 0;
        endcase
    endfunction

    function automatic trit_t xsum_model(input int av, input int bv);
        int sum;

        sum = av + bv;
        while (sum > 1) sum -= 3;
        while (sum < -1) sum += 3;
        return encode_model(sum);
    endfunction

    function automatic logic [2*TRITS-1:0] invalid_lane_model();
        logic [2*TRITS-1:0] out;

        for (int i = 0; i < TRITS; i++) begin
            out[2*i +: 2] = TRIT_INVALID;
        end
        return out;
    endfunction

    task automatic set_lane_trit(
        inout logic [2*TRITS-1:0] lane,
        input int                 pos,
        input trit_t              value
    );
        lane[2*pos +: 2] = value;
    endtask

    task automatic expect_trit(input trit_t got, input trit_t expected, input string label);
        if (got !== expected) begin
            $display("FAIL %s: got %b expected %b", label, got, expected);
            failures++;
        end
    endtask

    task automatic expect_bit(input logic got, input logic expected, input string label);
        if (got !== expected) begin
            $display("FAIL %s: got %b expected %b", label, got, expected);
            failures++;
        end
    endtask

    task automatic expect_lane(
        input logic [2*TRITS-1:0] got,
        input logic [2*TRITS-1:0] expected,
        input string              label
    );
        if (got !== expected) begin
            $display("FAIL %s: got %b expected %b", label, got, expected);
            failures++;
        end
    endtask

    task automatic test_single_trit_gates();
        int av;
        int bv;
        int cv;
        int total;
        int expected_sum;
        int expected_carry;

        for (av = -1; av <= 1; av++) begin
            a = encode_model(av);
            #1;
            expect_trit(y_neg, encode_model(-av), "trit_neg");
            expect_trit(y_tsel, encode_model(av), "trit_tsel identity arms");

            for (bv = -1; bv <= 1; bv++) begin
                b = encode_model(bv);
                #1;
                expect_trit(y_and, encode_model((av < bv) ? av : bv), "trit_and");
                expect_trit(y_or, encode_model((av > bv) ? av : bv), "trit_or");
                expect_trit(y_xsum, xsum_model(av, bv), "trit_xsum");
                expect_trit(y_xdiff, xsum_model(av, -bv), "trit_xdiff");

                for (cv = -1; cv <= 1; cv++) begin
                    c = encode_model(cv);
                    total = av + bv + cv;
                    expected_carry = 0;
                    expected_sum = total;
                    while (expected_sum > 1) begin
                        expected_sum -= 3;
                        expected_carry++;
                    end
                    while (expected_sum < -1) begin
                        expected_sum += 3;
                        expected_carry--;
                    end
                    #1;
                    expect_trit(y_sum, encode_model(expected_sum), "trit_full_adder sum");
                    expect_trit(y_carry, encode_model(expected_carry), "trit_full_adder carry");
                end
            end
        end

        a = TRIT_INVALID;
        b = TRIT_ZERO;
        c = TRIT_ZERO;
        #1;
        expect_trit(y_neg, TRIT_INVALID, "invalid neg");
        expect_trit(y_and, TRIT_INVALID, "invalid and");
        expect_trit(y_or, TRIT_INVALID, "invalid or");
        expect_trit(y_xsum, TRIT_INVALID, "invalid xsum");
        expect_trit(y_sum, TRIT_INVALID, "invalid full-adder sum");
        expect_trit(y_carry, TRIT_INVALID, "invalid full-adder carry");
        expect_trit(y_tsel, TRIT_INVALID, "invalid tsel condition");
    endtask

    task automatic test_lane_gates();
        logic [2*TRITS-1:0] expected;

        lane_a = '0;
        lane_b = '0;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(lane_a, i, TRIT_ZERO);
            set_lane_trit(lane_b, i, TRIT_ZERO);
        end
        set_lane_trit(lane_a, 0, TRIT_NEG);
        set_lane_trit(lane_a, 1, TRIT_ZERO);
        set_lane_trit(lane_a, 2, TRIT_POS);
        set_lane_trit(lane_a, 3, TRIT_NEG);
        set_lane_trit(lane_a, 4, TRIT_POS);

        set_lane_trit(lane_b, 0, TRIT_ZERO);
        set_lane_trit(lane_b, 1, TRIT_POS);
        set_lane_trit(lane_b, 2, TRIT_POS);
        set_lane_trit(lane_b, 3, TRIT_NEG);
        set_lane_trit(lane_b, 4, TRIT_NEG);
        #1;

        expected = lane_a;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_negate(lane_a[2*i +: 2]));
        end
        expect_bit(lane_valid_neg, 1'b1, "lane neg valid");
        expect_lane(lane_y_neg, expected, "lane neg");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_min(lane_a[2*i +: 2], lane_b[2*i +: 2]));
        end
        expect_bit(lane_valid_and, 1'b1, "lane and valid");
        expect_lane(lane_y_and, expected, "lane and");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_max(lane_a[2*i +: 2], lane_b[2*i +: 2]));
        end
        expect_bit(lane_valid_or, 1'b1, "lane or valid");
        expect_lane(lane_y_or, expected, "lane or");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_xsum(lane_a[2*i +: 2], lane_b[2*i +: 2]));
        end
        expect_bit(lane_valid_xsum, 1'b1, "lane xsum valid");
        expect_lane(lane_y_xsum, expected, "lane xsum");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(lane_a, i, TRIT_ZERO);
            set_lane_trit(lane_b, i, TRIT_ZERO);
        end
        set_lane_trit(lane_a, 0, TRIT_POS);
        set_lane_trit(lane_b, 0, TRIT_POS);
        set_lane_trit(expected, 0, TRIT_NEG);
        set_lane_trit(expected, 1, TRIT_POS);
        set_lane_trit(expected, 2, TRIT_ZERO);
        set_lane_trit(expected, 3, TRIT_ZERO);
        set_lane_trit(expected, 4, TRIT_ZERO);
        #1;
        expect_bit(lane_valid_add, 1'b1, "lane add valid");
        expect_lane(lane_y_add, expected, "lane add carry propagation");

        set_lane_trit(lane_a, 0, TRIT_NEG);
        set_lane_trit(lane_b, 0, TRIT_POS);
        set_lane_trit(expected, 0, TRIT_POS);
        set_lane_trit(expected, 1, TRIT_NEG);
        set_lane_trit(expected, 2, TRIT_ZERO);
        set_lane_trit(expected, 3, TRIT_ZERO);
        set_lane_trit(expected, 4, TRIT_ZERO);
        #1;
        expect_bit(lane_valid_sub, 1'b1, "lane sub valid");
        expect_lane(lane_y_sub, expected, "lane sub borrow propagation");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(lane_a, i, TRIT_POS);
            set_lane_trit(lane_b, i, TRIT_POS);
        end
        #1;
        expect_bit(lane_valid_add, 1'b0, "lane add overflow invalid");
        expect_lane(lane_y_add, invalid_lane_model(), "lane add overflow sentinel");

        set_lane_trit(lane_a, 2, TRIT_INVALID);
        #1;
        expect_bit(lane_valid_neg, 1'b0, "lane invalid input rejected");
        expect_lane(lane_y_neg, invalid_lane_model(), "lane invalid input sentinel");
    endtask

    task automatic test_lane_tsel();
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(lane_cond, i, (i == 0) ? TRIT_NEG : ((i == 1) ? TRIT_ZERO : TRIT_POS));
            set_lane_trit(lane_neg, i, TRIT_NEG);
            set_lane_trit(lane_zero, i, TRIT_ZERO);
            set_lane_trit(lane_pos, i, TRIT_POS);
        end
        #1;
        expect_bit(lane_valid_tsel, 1'b1, "lane tsel valid");
        expect_lane(lane_y_tsel, lane_cond, "lane tsel selects matching arms");

        set_lane_trit(lane_cond, 3, TRIT_INVALID);
        #1;
        expect_bit(lane_valid_tsel, 1'b0, "lane tsel invalid predicate rejected");
        expect_lane(lane_y_tsel, invalid_lane_model(), "lane tsel invalid predicate sentinel");
    endtask

    initial begin
        test_single_trit_gates();
        test_lane_gates();
        test_lane_tsel();

        if (failures == 0) begin
            $display("All SystemVerilog trit gate tests passed");
        end else begin
            $display("%0d SystemVerilog trit gate test failure(s)", failures);
            $fatal(1);
        end
        $finish;
    end
endmodule
