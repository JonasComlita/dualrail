export const correctedIntelligenceFiles: Record<string, Record<string, string>> = {
  "TC-SWE-001": {
    "src/validation.trit": `fn is_missing(reading: t40) -> t40 {
    if reading == -1000 { return 1; }
    return 0;
}

fn is_valid_reading(reading: t40) -> t40 {
    if reading == -1000 { return 1; }
    if reading < -729 { return 0; }
    if reading > 729 { return 0; }
    return 1;
}`,
    "src/compare.trit": `fn compare(a: t40, b: t40) -> t40 {
    if a < b { return -1; }
    if a > b { return 1; }
    return 0;
}`,
    "src/median.trit": `fn median3(a: t40, b: t40, c: t40) -> t40 {
    if a <= b {
        if b <= c { return b; }
        if a <= c { return c; }
        return a;
    }
    if a <= c { return a; }
    if b <= c { return c; }
    return b;
}`,
    "src/consensus.trit": `fn consensus(a: t40, b: t40, c: t40) -> t40 {
    if is_valid_reading(a) == 0 { return -1001; }
    if is_valid_reading(b) == 0 { return -1001; }
    if is_valid_reading(c) == 0 { return -1001; }
    var ma: t40 = is_missing(a);
    var mb: t40 = is_missing(b);
    var mc: t40 = is_missing(c);
    var missing: t40 = ma + mb + mc;
    if missing == 3 { return -1000; }
    if missing == 2 {
        if ma == 0 { return a; }
        if mb == 0 { return b; }
        return c;
    }
    if missing == 1 {
        if ma == 1 { return (b + c) / 2; }
        if mb == 1 { return (a + c) / 2; }
        return (a + b) / 2;
    }
    return median3(a, b, c);
}`,
  },
  "TC-SWE-002": {
    "src/trit_digits.trit": `fn trit_remainder(value: t40) -> t40 {
    var remainder: t40 = value;
    while remainder >= 3 {
        remainder = remainder - 3;
    }
    return remainder;
}

fn encoded_low(token: t40) -> t40 {
    return trit_remainder(token);
}

fn encoded_middle(token: t40) -> t40 {
    return trit_remainder(token / 3);
}

fn encoded_high(token: t40) -> t40 {
    return token / 9;
}

fn balanced_digit(encoded: t40) -> t40 {
    if encoded == 0 { return -1; }
    if encoded == 1 { return 0; }
    return 1;
}`,
    "src/parser.trit": `fn token_codec(token: t40, claimed_checksum: t40, mode: t40) -> t40 {
    if token < 0 { return -102; }
    if token > 26 { return -102; }
    var checksum: t40 = -3;
    if token == 1 { checksum = -2; }
    if token == 2 { checksum = -1; }
    if token == 3 { checksum = -2; }
    if token == 4 { checksum = -1; }
    if token == 5 { checksum = 0; }
    if token == 6 { checksum = -1; }
    if token == 7 { checksum = 0; }
    if token == 8 { checksum = 1; }
    if token == 9 { checksum = -2; }
    if token == 10 { checksum = -1; }
    if token == 11 { checksum = 0; }
    if token == 12 { checksum = -1; }
    if token == 13 { checksum = 0; }
    if token == 14 { checksum = 1; }
    if token == 15 { checksum = 0; }
    if token == 16 { checksum = 1; }
    if token == 17 { checksum = 2; }
    if token == 18 { checksum = -1; }
    if token == 19 { checksum = 0; }
    if token == 20 { checksum = 1; }
    if token == 21 { checksum = 0; }
    if token == 22 { checksum = 1; }
    if token == 23 { checksum = 2; }
    if token == 24 { checksum = 1; }
    if token == 25 { checksum = 2; }
    if token == 26 { checksum = 3; }
    if claimed_checksum != checksum { return -100; }
    if mode == 0 { return token; }
    if mode == 1 { return token - 13; }
    if mode == 2 {
        if token == 0 { return 0; }
        if token == 1 { return 9; }
        if token == 2 { return 18; }
        if token == 3 { return 1; }
        if token == 4 { return 10; }
        if token == 5 { return 19; }
        if token == 6 { return 2; }
        if token == 7 { return 11; }
        if token == 8 { return 20; }
        if token == 9 { return 3; }
        if token == 10 { return 12; }
        if token == 11 { return 21; }
        if token == 12 { return 4; }
        if token == 13 { return 13; }
        if token == 14 { return 22; }
        if token == 15 { return 5; }
        if token == 16 { return 14; }
        if token == 17 { return 23; }
        if token == 18 { return 6; }
        if token == 19 { return 15; }
        if token == 20 { return 24; }
        if token == 21 { return 7; }
        if token == 22 { return 16; }
        if token == 23 { return 25; }
        if token == 24 { return 8; }
        if token == 25 { return 17; }
        return 26;
    }
    return -101;
}`,
  },
  "TC-SWE-003": {
    "src/alignment.trit": `fn valid_alignment(alignment: t40) -> t40 {
    if alignment == 1 { return 1; }
    if alignment == 3 { return 1; }
    if alignment == 9 { return 1; }
    return 0;
}

fn is_aligned(address: t40, alignment: t40) -> t40 {
    var remainder: t40 = address;
    while remainder >= alignment {
        remainder = remainder - alignment;
    }
    if remainder == 0 { return 1; }
    return 0;
}`,
    "src/pointer_guard.trit": `fn checked_span(address: t40, length: t40, alignment: t40) -> t40 {
    if valid_alignment(alignment) == 0 { return -2; }
    if address < 0 { return -1; }
    if length <= 0 { return -1; }
    if address >= 27 { return -1; }
    if address + length > 27 { return -1; }
    if is_aligned(address, alignment) == 0 { return -3; }
    return address + length - 1;
}`,
  },
  "TC-SWE-004": {
    "src/events.trit": `fn valid_event(event: t40) -> t40 {
    if event < -2 { return 0; }
    if event > 2 { return 0; }
    return 1;
}

fn apply_event(state: t40, event: t40) -> t40 {
    if event == -2 { return 0; }
    if event == -1 {
        if state == -9 { return -9; }
        return state - 1;
    }
    if event == 0 { return state; }
    if event == 1 {
        if state == 9 { return 9; }
        return state + 1;
    }
    return 0 - state;
}`,
    "src/state_transition.trit": `fn apply_two_events(state: t40, event1: t40, event2: t40) -> t40 {
    if state < -9 { return -28; }
    if state > 9 { return -28; }
    if valid_event(event1) == 0 { return -27; }
    if valid_event(event2) == 0 { return -27; }
    return apply_event(apply_event(state, event1), event2);
}`,
  },
  "TC-SWE-005": {
    "src/result_policy.trit": `fn valid_policy(policy: t40) -> t40 {
    if policy < 0 { return 0; }
    if policy > 3 { return 0; }
    return 1;
}

fn apply_policy(value: t40, policy: t40) -> t40 {
    if policy == 0 { return value; }
    if policy == 1 {
        if value < 0 { return -3; }
        return value;
    }
    if policy == 2 {
        if value < -729 { return -729; }
        if value > 729 { return 729; }
        return value;
    }
    if value == 0 { return 0; }
    return 1;
}`,
    "src/syscall_adapter.trit": `fn adapt_syscall(status: t40, value: t40, policy: t40) -> t40 {
    if status < 0 { return status; }
    if status > 1 { return -2; }
    if status == 1 {
        if value < 0 { return -3; }
    }
    if valid_policy(policy) == 0 { return -4; }
    return apply_policy(value, policy);
}`,
  },
};
