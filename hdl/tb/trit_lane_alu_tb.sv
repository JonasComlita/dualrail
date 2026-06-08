`timescale 1ns/1ps

module trit_lane_alu_tb;
    import trit_pkg::*;

    localparam int TRITS = 5;
    localparam trit_t TB_NEG     = 2'b00;
    localparam trit_t TB_ZERO    = 2'b01;
    localparam trit_t TB_POS     = 2'b10;
    localparam trit_t TB_INVALID = 2'b11;

    trit_lane_op_t op;
    logic [2*TRITS-1:0] src_a;
    logic [2*TRITS-1:0] src_b;
    logic [2*TRITS-1:0] src_c;
    logic [2*TRITS-1:0] src_d;
    logic [2*TRITS-1:0] lane_y;
    logic valid;
    logic fault;
    int failures = 0;

    trit_lane_alu #(.TRITS(TRITS)) dut (
        .op_i(op),
        .src_a_i(src_a),
        .src_b_i(src_b),
        .src_c_i(src_c),
        .src_d_i(src_d),
        .lane_o(lane_y),
        .valid_o(valid),
        .fault_o(fault)
    );

    task automatic set_lane_trit(
        inout logic [2*TRITS-1:0] lane,
        input int                 pos,
        input trit_t              value
    );
        lane[2*pos +: 2] = value;
    endtask

    function automatic logic [2*TRITS-1:0] invalid_lane_model();
        logic [2*TRITS-1:0] out;

        for (int i = 0; i < TRITS; i++) begin
            out[2*i +: 2] = TB_INVALID;
        end
        return out;
    endfunction

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

    task automatic expect_status(
        input logic  got_valid,
        input logic  got_fault,
        input logic  expected_valid,
        input string label
    );
        if (got_valid !== expected_valid || got_fault !== !expected_valid) begin
            $display(
                "FAIL %s status: valid/fault got %b/%b expected %b/%b",
                label,
                got_valid,
                got_fault,
                expected_valid,
                !expected_valid
            );
            failures++;
        end
    endtask

    task automatic init_operands();
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(src_a, i, TB_ZERO);
            set_lane_trit(src_b, i, TB_ZERO);
            set_lane_trit(src_c, i, TB_ZERO);
            set_lane_trit(src_d, i, TB_ZERO);
        end

        set_lane_trit(src_a, 0, TB_NEG);
        set_lane_trit(src_a, 1, TB_ZERO);
        set_lane_trit(src_a, 2, TB_POS);
        set_lane_trit(src_a, 3, TB_NEG);
        set_lane_trit(src_a, 4, TB_POS);

        set_lane_trit(src_b, 0, TB_ZERO);
        set_lane_trit(src_b, 1, TB_POS);
        set_lane_trit(src_b, 2, TB_POS);
        set_lane_trit(src_b, 3, TB_NEG);
        set_lane_trit(src_b, 4, TB_NEG);
    endtask

    task automatic test_logic_ops();
        logic [2*TRITS-1:0] expected;

        init_operands();

        op = TRIT_LANE_OP_NEG;
        #1;
        expected = src_a;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_pkg::trit_negate(src_a[2*i +: 2]));
        end
        expect_status(valid, fault, 1'b1, "alu neg");
        expect_lane(lane_y, expected, "alu neg");

        op = TRIT_LANE_OP_AND;
        #1;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_pkg::trit_min(src_a[2*i +: 2], src_b[2*i +: 2]));
        end
        expect_status(valid, fault, 1'b1, "alu and");
        expect_lane(lane_y, expected, "alu and");

        op = TRIT_LANE_OP_OR;
        #1;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_pkg::trit_max(src_a[2*i +: 2], src_b[2*i +: 2]));
        end
        expect_status(valid, fault, 1'b1, "alu or");
        expect_lane(lane_y, expected, "alu or");

        op = TRIT_LANE_OP_XSUM;
        #1;
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(expected, i, trit_pkg::trit_xsum(src_a[2*i +: 2], src_b[2*i +: 2]));
        end
        expect_status(valid, fault, 1'b1, "alu xsum");
        expect_lane(lane_y, expected, "alu xsum");
    endtask

    task automatic test_add_sub_tsel();
        logic [2*TRITS-1:0] expected;

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(src_a, i, TB_ZERO);
            set_lane_trit(src_b, i, TB_ZERO);
            set_lane_trit(src_c, i, TB_ZERO);
            set_lane_trit(src_d, i, TB_ZERO);
            set_lane_trit(expected, i, TB_ZERO);
        end

        set_lane_trit(src_a, 0, TB_POS);
        set_lane_trit(src_b, 0, TB_POS);
        set_lane_trit(expected, 0, TB_NEG);
        set_lane_trit(expected, 1, TB_POS);
        op = TRIT_LANE_OP_ADD;
        #1;
        expect_status(valid, fault, 1'b1, "alu add");
        expect_lane(lane_y, expected, "alu add carry");

        set_lane_trit(src_a, 0, TB_NEG);
        set_lane_trit(src_b, 0, TB_POS);
        set_lane_trit(expected, 0, TB_POS);
        set_lane_trit(expected, 1, TB_NEG);
        op = TRIT_LANE_OP_SUB;
        #1;
        expect_status(valid, fault, 1'b1, "alu sub");
        expect_lane(lane_y, expected, "alu sub borrow");

        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(src_a, i, (i == 0) ? TB_NEG : ((i == 1) ? TB_ZERO : TB_POS));
            set_lane_trit(src_b, i, TB_NEG);
            set_lane_trit(src_c, i, TB_ZERO);
            set_lane_trit(src_d, i, TB_POS);
            set_lane_trit(expected, i, src_a[2*i +: 2]);
        end
        op = TRIT_LANE_OP_TSEL;
        #1;
        expect_status(valid, fault, 1'b1, "alu tsel");
        expect_lane(lane_y, expected, "alu tsel");
    endtask

    task automatic test_faults();
        for (int i = 0; i < TRITS; i++) begin
            set_lane_trit(src_a, i, TB_POS);
            set_lane_trit(src_b, i, TB_POS);
            set_lane_trit(src_c, i, TB_ZERO);
            set_lane_trit(src_d, i, TB_ZERO);
        end

        op = TRIT_LANE_OP_ADD;
        #1;
        expect_status(valid, fault, 1'b0, "alu add overflow");
        expect_lane(lane_y, invalid_lane_model(), "alu add overflow");

        set_lane_trit(src_a, 2, TB_INVALID);
        op = TRIT_LANE_OP_NEG;
        #1;
        expect_status(valid, fault, 1'b0, "alu invalid source");
        expect_lane(lane_y, invalid_lane_model(), "alu invalid source");

        op = trit_lane_op_t'(3'd7);
        #1;
        expect_status(valid, fault, 1'b0, "alu invalid op");
        expect_lane(lane_y, invalid_lane_model(), "alu invalid op");
    endtask

    initial begin
        test_logic_ops();
        test_add_sub_tsel();
        test_faults();

        if (failures == 0) begin
            $display("All SystemVerilog trit lane ALU tests passed");
        end else begin
            $display("%0d SystemVerilog trit lane ALU test failure(s)", failures);
            $fatal(1);
        end
        $finish;
    end
endmodule
