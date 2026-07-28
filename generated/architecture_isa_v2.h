// Generated-contract companion for versioned ISA encoding.
// Included by ternary_isa.h after Opcode and TritWord27 are defined.

[[nodiscard]] inline int decodeUnsignedField(const TritWord27& word,
                                             int lsb,
                                             int width) {
    int value = 0;
    int place = 1;
    for (int i = 0; i < width; ++i) {
        value += (word.getTrit(lsb + i) + 1) * place;
        place *= 3;
    }
    return value;
}

inline void encodeUnsignedField(TritWord27& word,
                                int lsb,
                                int width,
                                int value) {
    int capacity = 1;
    for (int i = 0; i < width; ++i) capacity *= 3;
    if (value < 0 || value >= capacity) {
        throw std::out_of_range("unsigned ternary field exceeds encoding range");
    }
    for (int i = 0; i < width; ++i) {
        const int digit = value % 3;
        word.setTrit(lsb + i, static_cast<int8_t>(digit - 1));
        value /= 3;
    }
}

[[nodiscard]] inline int v2DirectOpcode(Opcode opcode) {
    const int semantic = static_cast<int>(opcode);
    if (semantic >= 0 && semantic <= 14) return semantic;
    if (semantic >= 16 && semantic <= 26) return semantic;
    switch (opcode) {
        case Opcode::TINV: return architecture::v2::OPCODE_NEG;
        case Opcode::CALLR: return architecture::v2::OPCODE_CALLR;
        case Opcode::JMPR: return architecture::v2::OPCODE_JMPR;
        case Opcode::SYSCALL: return architecture::v2::OPCODE_SYSCALL;
        case Opcode::FENCE: return architecture::v2::OPCODE_FENCE;
        case Opcode::CSRR: return architecture::v2::OPCODE_CSRR;
        case Opcode::CSRW: return architecture::v2::OPCODE_CSRW;
        case Opcode::ERET: return architecture::v2::OPCODE_ERET;
        case Opcode::CSRRW: return architecture::v2::OPCODE_CSRRW;
        case Opcode::TLDR: return architecture::v2::OPCODE_TLDR;
        case Opcode::TSTR: return architecture::v2::OPCODE_TSTR;
        case Opcode::WAIT: return architecture::v2::OPCODE_WAIT;
        default: return -1;
    }
}

[[nodiscard]] inline Opcode decodeV2DirectOpcode(int raw_opcode) {
    if (raw_opcode >= 0 && raw_opcode <= 14) return static_cast<Opcode>(raw_opcode);
    if (raw_opcode >= 16 && raw_opcode <= 26) return static_cast<Opcode>(raw_opcode);
    switch (raw_opcode) {
        case architecture::v2::OPCODE_CALLR: return Opcode::CALLR;
        case architecture::v2::OPCODE_JMPR: return Opcode::JMPR;
        case architecture::v2::OPCODE_SYSCALL: return Opcode::SYSCALL;
        case architecture::v2::OPCODE_FENCE: return Opcode::FENCE;
        case architecture::v2::OPCODE_CSRR: return Opcode::CSRR;
        case architecture::v2::OPCODE_CSRW: return Opcode::CSRW;
        case architecture::v2::OPCODE_ERET: return Opcode::ERET;
        case architecture::v2::OPCODE_CSRRW: return Opcode::CSRRW;
        case architecture::v2::OPCODE_TLDR: return Opcode::TLDR;
        case architecture::v2::OPCODE_TSTR: return Opcode::TSTR;
        case architecture::v2::OPCODE_WAIT: return Opcode::WAIT;
        default: return Opcode::RESERVED;
    }
}

[[nodiscard]] inline Opcode decodeV2ExtensionSelector(int selector) {
    switch (selector) {
        case 27: return Opcode::TLADD;
        case 28: return Opcode::TLSUB;
        case 29: return Opcode::TLNEG;
        case 30: return Opcode::TLAND;
        case 31: return Opcode::TLOR;
        case 32: return Opcode::VADD;
        case 33: return Opcode::VSUB;
        case 34: return Opcode::VNEG;
        case 35: return Opcode::VMUL;
        case 36: return Opcode::VDIV;
        case 37: return Opcode::VCMP;
        case 38: return Opcode::VSEL;
        case 39: return Opcode::VLOAD;
        case 40: return Opcode::VSTORE;
        case 41: return Opcode::VBCAST;
        case 42: return Opcode::VLEN;
        case 43: return Opcode::ACLR;
        case 44: return Opcode::ALOAD;
        case 45: return Opcode::AADD;
        case 46: return Opcode::ASUB;
        case 47: return Opcode::AMUL;
        case 48: return Opcode::ASTORE;
        case 49: return Opcode::VDOT;
        case 50: return Opcode::VMAC;
        case 51: return Opcode::VACT;
        case 52: return Opcode::VPACK;
        case 53: return Opcode::VUNPACK;
        case 54: return Opcode::VPERMUTE;
        case 55: return Opcode::VBLEND;
        case 56: return Opcode::VSWAP;
        case 57: return Opcode::VGATHER;
        case 58: return Opcode::VSCATTER;
        case 59: return Opcode::TWCMP;
        case 62: return Opcode::TMOD;
        case 63: return Opcode::TLSHIFT;
        case 64: return Opcode::TRSHIFT;
        case 65: return Opcode::TMAC;
        case 66: return Opcode::TCOUNT;
        case 67: return Opcode::TSCAN;
        case 68: return Opcode::TCLAMP;
        case 71: return Opcode::VSUM;
        case 72: return Opcode::VHMIN;
        case 73: return Opcode::VHMAX;
        case 80: return Opcode::TLBINV;
        default: return Opcode::RESERVED;
    }
}

[[nodiscard]] inline int v2ExtensionSelector(Opcode opcode) {
    if (opcode == Opcode::TLBINV) return architecture::v2::EXT_TLBINV;
    const int selector = static_cast<int>(opcode);
    return decodeV2ExtensionSelector(selector) == opcode ? selector : -1;
}
