// Sole public instruction-word codec for the generated ISA-v2 wire contract.
// InstructionWord's encodeSemantic*/decodeSemantic helpers are private staging
// primitives used underneath this versioned boundary.

struct VersionedInstructionCodec {
    [[nodiscard]] static InstructionWord decode(
        const TritWord27& word,
        IsaEncodingVersion version) {

        InstructionWord decoded = InstructionWord::decodeSemantic(word);
        if (decoded.malformed) return decoded;

        const int raw_opcode =
            decodeUnsignedField(word, FIELD_OP_LSB, FIELD_OP_W);
        bool extension = raw_opcode == architecture::v2::ESCAPE_OPCODE;
        int selector = -1;

        if (!extension) {
            decoded.opcode = decodeV2DirectOpcode(raw_opcode);
        } else {
            switch (decoded.fmt) {
                case InstructionFormat::R_TYPE:
                    selector = decodeUnsignedField(
                        word,
                        0,
                        4);
                    decoded.opcode = decodeV2ExtensionSelector(selector);
                    if (decoded.opcode != Opcode::TWCMP &&
                        decoded.opcode != Opcode::TCLAMP &&
                        decoded.opcode != Opcode::VSEL &&
                        decoded.opcode != Opcode::VBLEND) {
                        selector = decodeUnsignedField(word, 0, 10);
                        decoded.opcode = decodeV2ExtensionSelector(selector);
                    }
                    break;
                case InstructionFormat::I_TYPE: {
                    const int vector_selector =
                        decodeUnsignedField(word, 9, 4);
                    Opcode vector_op =
                        decodeV2ExtensionSelector(vector_selector);
                    if (vector_op == Opcode::VLOAD ||
                        vector_op == Opcode::VSTORE) {
                        selector = vector_selector;
                        decoded.opcode = vector_op;
                    } else {
                        selector = decodeUnsignedField(word, 12, 4);
                        decoded.opcode =
                            decodeV2ExtensionSelector(selector);
                    }
                    break;
                }
                case InstructionFormat::B_TYPE:
                    selector = decodeUnsignedField(word, 15, 4);
                    decoded.opcode = decodeV2ExtensionSelector(selector);
                    break;
                default:
                    decoded.opcode = Opcode::RESERVED;
                    break;
            }
        }

        if (decoded.opcode == Opcode::RESERVED) return decoded;

        decoded.r4_layout = false;
        decoded.r5_layout = false;
        decoded.rs3 = 0;
        decoded.rcond = decoded.rneg = decoded.rzero = decoded.rpos = 0;

        switch (decoded.fmt) {
            case InstructionFormat::R_TYPE:
                if (decoded.opcode == Opcode::TSEL ||
                    decoded.opcode == Opcode::VSEL ||
                    decoded.opcode == Opcode::VBLEND) {
                    decoded.r5_layout = true;
                    decoded.rd = reg(word, FIELD_R5_RD_LSB, FIELD_R5_RD_W);
                    decoded.rcond =
                        reg(word, FIELD_R5_COND_LSB, FIELD_R5_COND_W);
                    decoded.rneg =
                        reg(word, FIELD_R5_NEG_LSB, FIELD_R5_NEG_W);
                    decoded.rzero =
                        reg(word, FIELD_R5_ZERO_LSB, FIELD_R5_ZERO_W);
                    decoded.rpos =
                        reg(word, FIELD_R5_POS_LSB, FIELD_R5_POS_W);
                    decoded.rs1 = decoded.rcond;
                    decoded.rs2 = decoded.rneg;
                    decoded.func =
                        (extension &&
                         (decoded.opcode == Opcode::VSEL ||
                          decoded.opcode == Opcode::VBLEND))
                            ? reg(word, 4, 3)
                            : ((decoded.opcode == Opcode::VSEL ||
                                decoded.opcode == Opcode::VBLEND)
                                   ? reg(word,
                                         FIELD_R5_FUNC_LSB,
                                         FIELD_R5_FUNC_W)
                                   : FUNC_DEFAULT);
                } else if (decoded.opcode == Opcode::TWCMP ||
                           decoded.opcode == Opcode::TCLAMP ||
                           decoded.opcode == Opcode::TSTR) {
                    decoded.r4_layout = true;
                    decoded.rd =
                        reg(word, FIELD_R4_RD_LSB, FIELD_R4_RD_W);
                    decoded.rs1 =
                        reg(word, FIELD_R4_RS1_LSB, FIELD_R4_RS1_W);
                    decoded.rs2 =
                        reg(word, FIELD_R4_RS2_LSB, FIELD_R4_RS2_W);
                    decoded.rs3 =
                        reg(word, FIELD_R4_RS3_LSB, FIELD_R4_RS3_W);
                    decoded.func =
                        reg(word, FIELD_R4_FUNC_LSB, FIELD_R4_FUNC_W);
                } else {
                    decoded.rd = reg(word, FIELD_RD_LSB, FIELD_RD_W);
                    decoded.rs1 = reg(word, FIELD_RS1_LSB, FIELD_RS1_W);
                    decoded.rs2 = reg(word, FIELD_RS2_LSB, FIELD_RS2_W);
                    decoded.func =
                        reg(word, FIELD_FUNC_LSB, FIELD_FUNC_W);
                }
                break;
            case InstructionFormat::I_TYPE:
                decoded.rd = reg(word, FIELD_RD_LSB, FIELD_RD_W);
                decoded.rs1 = reg(word, FIELD_RS1_LSB, FIELD_RS1_W);
                decoded.rs_store = decoded.rd;
                if (extension &&
                    (decoded.opcode == Opcode::VLOAD ||
                     decoded.opcode == Opcode::VSTORE)) {
                    decoded.func = reg(word, 13, 3);
                    decoded.imm = decodeSigned(word, 0, 9);
                } else if (extension) {
                    decoded.imm = decodeSigned(word, 0, 12);
                } else {
                    decoded.imm =
                        decodeSigned(word, FIELD_IMM16_LSB, FIELD_IMM16_W);
                }
                break;
            case InstructionFormat::B_TYPE:
                decoded.rs_branch =
                    reg(word, FIELD_BRS_LSB, FIELD_BRS_W);
                decoded.offset = extension
                    ? decodeSigned(word, 0, 15)
                    : decodeSigned(word, FIELD_OFF19_LSB, FIELD_OFF19_W);
                break;
            default:
                decoded.malformed = true;
                break;
        }

        validateRegisters(decoded);
        return decoded;
    }

    [[nodiscard]] static TritWord27 encodeR(
        Opcode opcode,
        uint8_t rd,
        uint8_t rs1,
        uint8_t rs2,
        uint8_t func,
        IsaEncodingVersion version) {

        TritWord27 word =
            InstructionWord::encodeSemanticR(opcode, rd, rs1, rs2, func);
        encodeV2Opcode(word, opcode, 0, 10);
        return word;
    }

    [[nodiscard]] static TritWord27 encodeR4(
        Opcode opcode,
        uint8_t rd,
        uint8_t rs1,
        uint8_t rs2,
        uint8_t rs3,
        uint8_t func,
        IsaEncodingVersion version) {

        TritWord27 word =
            InstructionWord::encodeSemanticR4(
                opcode, rd, rs1, rs2, rs3, func);
        encodeV2Opcode(word, opcode, 0, 4);
        return word;
    }

    [[nodiscard]] static TritWord27 encodeR5(
        Opcode opcode,
        uint8_t rd,
        uint8_t rcond,
        uint8_t rneg,
        uint8_t rzero,
        uint8_t rpos,
        uint8_t func,
        IsaEncodingVersion version) {

        TritWord27 word = InstructionWord::encodeSemanticR5(
            opcode, rd, rcond, rneg, rzero, rpos, func);
        const int direct = v2DirectOpcode(opcode);
        if (direct >= 0) {
            setRawOpcode(word, direct);
            return word;
        }
        const int selector = v2ExtensionSelector(opcode);
        if (selector < 0) throwUnsupported(opcode);
        setRawOpcode(word, architecture::v2::ESCAPE_OPCODE);
        encodeUnsignedField(word, 0, 4, selector);
        word.setField(4, 3, static_cast<int>(func) - REG_FIELD_OFFSET);
        return word;
    }

    [[nodiscard]] static TritWord27 encodeI(
        Opcode opcode,
        uint8_t rd,
        uint8_t rs1,
        int immediate,
        IsaEncodingVersion version) {

        TritWord27 word =
            InstructionWord::encodeSemanticI(opcode, rd, rs1, immediate);
        const int direct = v2DirectOpcode(opcode);
        if (direct >= 0) {
            setRawOpcode(word, direct);
            return word;
        }
        const int selector = v2ExtensionSelector(opcode);
        if (selector < 0) throwUnsupported(opcode);
        setRawOpcode(word, architecture::v2::ESCAPE_OPCODE);
        encodeSigned(word, 0, 12, immediate);
        encodeUnsignedField(word, 12, 4, selector);
        return word;
    }

    [[nodiscard]] static TritWord27 encodeVectorMemory(
        Opcode opcode,
        uint8_t vector_register,
        uint8_t base,
        int immediate,
        uint8_t func,
        IsaEncodingVersion version) {

        TritWord27 word = InstructionWord::encodeSemanticVectorMemory(
            opcode, vector_register, base, immediate, func);
        const int selector = v2ExtensionSelector(opcode);
        if (selector < 0) throwUnsupported(opcode);
        setRawOpcode(word, architecture::v2::ESCAPE_OPCODE);
        encodeSigned(word, 0, 9, immediate);
        encodeUnsignedField(word, 9, 4, selector);
        word.setField(13, 3, static_cast<int>(func) - REG_FIELD_OFFSET);
        return word;
    }

    [[nodiscard]] static TritWord27 encodeB(
        Opcode opcode,
        uint8_t branch_register,
        int offset,
        IsaEncodingVersion version) {

        TritWord27 word =
            InstructionWord::encodeSemanticB(opcode, branch_register, offset);
        const int direct = v2DirectOpcode(opcode);
        if (direct >= 0) {
            setRawOpcode(word, direct);
            return word;
        }
        const int selector = v2ExtensionSelector(opcode);
        if (selector < 0) throwUnsupported(opcode);
        setRawOpcode(word, architecture::v2::ESCAPE_OPCODE);
        encodeSigned(word, 0, 15, offset);
        encodeUnsignedField(word, 15, 4, selector);
        return word;
    }

private:
    [[nodiscard]] static uint8_t reg(
        const TritWord27& word,
        int lsb,
        int width) {
        return static_cast<uint8_t>(
            word.getField(lsb, width) + REG_FIELD_OFFSET);
    }

    static void setRawOpcode(TritWord27& word, int raw_opcode) {
        encodeUnsignedField(
            word, FIELD_OP_LSB, FIELD_OP_W, raw_opcode);
    }

    static void encodeV2Opcode(
        TritWord27& word,
        Opcode opcode,
        int selector_lsb,
        int selector_width) {
        const int direct = v2DirectOpcode(opcode);
        if (direct >= 0) {
            setRawOpcode(word, direct);
            return;
        }
        const int selector = v2ExtensionSelector(opcode);
        if (selector < 0) throwUnsupported(opcode);
        setRawOpcode(word, architecture::v2::ESCAPE_OPCODE);
        encodeUnsignedField(
            word, selector_lsb, selector_width, selector);
    }

    [[noreturn]] static void throwUnsupported(Opcode opcode) {
        throw std::invalid_argument(
            "opcode has no ISA v2 direct or extension encoding: " +
            std::to_string(static_cast<int>(opcode)));
    }

    static void validateRegisters(InstructionWord& decoded) {
        auto check = [&](uint8_t value) {
            if (value >= REG_COUNT) decoded.malformed = true;
        };
        if (decoded.fmt == InstructionFormat::R_TYPE) {
            if (decoded.r5_layout) {
                check(decoded.rd);
                check(decoded.rcond);
                check(decoded.rneg);
                check(decoded.rzero);
                check(decoded.rpos);
            } else if (decoded.r4_layout) {
                check(decoded.rd);
                check(decoded.rs1);
                check(decoded.rs2);
                check(decoded.rs3);
            } else {
                check(decoded.rd);
                check(decoded.rs1);
                check(decoded.rs2);
            }
        } else if (decoded.fmt == InstructionFormat::I_TYPE) {
            check(decoded.rd);
            check(decoded.rs1);
        } else if (decoded.fmt == InstructionFormat::B_TYPE) {
            check(decoded.rs_branch);
        }
    }
};
