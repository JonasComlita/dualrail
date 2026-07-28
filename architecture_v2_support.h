#pragma once

// Included by ternary_isa.h after VersionedInstructionCodec is defined.

[[nodiscard]] inline constexpr std::uint64_t featureBit(int trit) {
    return std::uint64_t{1} << static_cast<unsigned>(trit);
}


[[nodiscard]] inline constexpr long long featureWordNumeric(
    std::uint64_t features) {
    long long value = 0;
    long long place = 1;
    for (int trit = 0; trit <= architecture::v2::FEATURE_WIDE_T50;
         ++trit) {
        if ((features & featureBit(trit)) != 0) value += place;
        place *= 3;
    }
    return value;
}
[[nodiscard]] inline std::uint64_t requiredV2Features(
    const InstructionWord& instruction) {
    std::uint64_t features = 0;
    switch (instruction.opcode) {
        case Opcode::TWCMP:
        case Opcode::TMOD:
        case Opcode::TLSHIFT:
        case Opcode::TRSHIFT:
        case Opcode::TMAC:
        case Opcode::TCOUNT:
        case Opcode::TSCAN:
        case Opcode::TCLAMP:
            features |= featureBit(
                architecture::v2::FEATURE_SCALAR_ADVANCED);
            break;
        case Opcode::TLADD:
        case Opcode::TLSUB:
        case Opcode::TLNEG:
        case Opcode::TLAND:
        case Opcode::TLOR:
            features |= featureBit(architecture::v2::FEATURE_LANE);
            break;
        case Opcode::VADD:
        case Opcode::VSUB:
        case Opcode::VNEG:
        case Opcode::VMUL:
        case Opcode::VDIV:
        case Opcode::VCMP:
        case Opcode::VSEL:
        case Opcode::VLOAD:
        case Opcode::VSTORE:
        case Opcode::VBCAST:
        case Opcode::VLEN:
        case Opcode::VPACK:
        case Opcode::VUNPACK:
        case Opcode::VPERMUTE:
        case Opcode::VBLEND:
        case Opcode::VSWAP:
        case Opcode::VGATHER:
        case Opcode::VSCATTER:
        case Opcode::VSUM:
        case Opcode::VHMIN:
        case Opcode::VHMAX:
            features |= featureBit(architecture::v2::FEATURE_VECTOR);
            break;
        case Opcode::ACLR:
        case Opcode::ALOAD:
        case Opcode::AADD:
        case Opcode::ASUB:
        case Opcode::AMUL:
        case Opcode::ASTORE:
        case Opcode::VDOT:
        case Opcode::VMAC:
        case Opcode::VACT:
            features |= featureBit(
                architecture::v2::FEATURE_ACCUMULATOR_AI);
            break;
        case Opcode::FENCE:
        case Opcode::TLDR:
        case Opcode::TSTR:
            features |= featureBit(architecture::v2::FEATURE_ATOMICS);
            break;
        case Opcode::TLBINV:
            features |= featureBit(architecture::v2::FEATURE_MMU);
            break;
        case Opcode::WAIT:
            features |= featureBit(architecture::v2::FEATURE_WAIT);
            break;
        default:
            break;
    }

    if (instruction.func == FUNC_T50 ||
        instruction.func == FUNC_L50) {
        features |= featureBit(architecture::v2::FEATURE_WIDE_T50);
    }
    return features;
}

[[nodiscard]] inline TritWord27 transcodeV1InstructionToV2(
    const TritWord27& word) {
    const InstructionWord decoded =
        VersionedInstructionCodec::decode(
            word, IsaEncodingVersion::V1);
    if (decoded.malformed || decoded.opcode == Opcode::RESERVED) {
        throw std::invalid_argument(
            "cannot transcode malformed or reserved v1 instruction");
    }

    if (decoded.r5_layout) {
        return VersionedInstructionCodec::encodeR5(
            decoded.opcode, decoded.rd, decoded.rcond, decoded.rneg,
            decoded.rzero, decoded.rpos, decoded.func,
            IsaEncodingVersion::V2);
    }
    if (decoded.r4_layout) {
        return VersionedInstructionCodec::encodeR4(
            decoded.opcode, decoded.rd, decoded.rs1, decoded.rs2,
            decoded.rs3, decoded.func, IsaEncodingVersion::V2);
    }
    if (decoded.fmt == InstructionFormat::I_TYPE) {
        if (decoded.opcode == Opcode::VLOAD ||
            decoded.opcode == Opcode::VSTORE) {
            return VersionedInstructionCodec::encodeVectorMemory(
                decoded.opcode, decoded.rd, decoded.rs1, decoded.imm,
                decoded.func, IsaEncodingVersion::V2);
        }
        return VersionedInstructionCodec::encodeI(
            decoded.opcode, decoded.rd, decoded.rs1, decoded.imm,
            IsaEncodingVersion::V2);
    }
    if (decoded.fmt == InstructionFormat::B_TYPE) {
        return VersionedInstructionCodec::encodeB(
            decoded.opcode, decoded.rs_branch, decoded.offset,
            IsaEncodingVersion::V2);
    }
    return VersionedInstructionCodec::encodeR(
        decoded.opcode, decoded.rd, decoded.rs1, decoded.rs2,
        decoded.func, IsaEncodingVersion::V2);
}
