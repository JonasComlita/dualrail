"""Pure-Python implementation of the Trit symbolic encoding contract.

The C++ header :mod:`ternary_symbolic_encoding.h` and this module intentionally
share their tables and canonical spellings.  Keeping this dependency-free lets
``trit_tool`` inspect a word even before a C++ build is available.
"""

from __future__ import annotations

from typing import Iterable

INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1

TASCII81_TABLE = (
    " \t\n\"',.:;!-_+=/[]{}"
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
)
BASE27_ALPHABET = "0123456789abcdefghijklmnopq"
BASE81_ALPHABET = (
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?"
)

assert len(TASCII81_TABLE) == 81
assert len(BASE27_ALPHABET) == 27
assert len(BASE81_ALPHABET) == 81


def ascii_to_tascii81(value: str) -> list[int]:
    out: list[int] = []
    for char in value:
        if ord(char) > 0x7F or char not in TASCII81_TABLE:
            raise ValueError("TASCII-81 only accepts ASCII characters in its fixed table")
        out.append(TASCII81_TABLE.index(char))
    return out


def tascii81_to_ascii(values: Iterable[int]) -> str:
    chars: list[str] = []
    for value in values:
        if type(value) is not int or not 0 <= value < len(TASCII81_TABLE):
            raise ValueError("invalid TASCII-81 symbol index")
        chars.append(TASCII81_TABLE[value])
    return "".join(chars)


def validate_utf8(value: str | bytes) -> bool:
    try:
        if isinstance(value, bytes):
            value.decode("utf-8", "strict")
        else:
            value.encode("utf-8", "strict")
        return True
    except UnicodeError:
        return False


def utf8_to_codepoints(value: str | bytes) -> list[int]:
    text = value.decode("utf-8", "strict") if isinstance(value, bytes) else value
    return [ord(char) for char in text]


def codepoints_to_utf8(values: Iterable[int]) -> bytes:
    chars: list[str] = []
    for value in values:
        if type(value) is not int or not 0 <= value <= 0x10FFFF:
            raise ValueError("invalid Unicode scalar value")
        if 0xD800 <= value <= 0xDFFF:
            raise ValueError("surrogate code point is not valid UTF-8")
        chars.append(chr(value))
    return "".join(chars).encode("utf-8")


def hex_encode(value: bytes | bytearray | Iterable[int], prefix: bool = False) -> str:
    data = bytes(value)
    result = data.hex().upper()
    return ("0x" if prefix else "") + result


def hex_decode(value: str) -> bytes:
    text = value[2:] if value.startswith(("0x", "0X")) else value
    if len(text) % 2:
        raise ValueError("hex string has odd length")
    digits = "0123456789abcdefABCDEF"
    if any(char not in digits for char in text):
        raise ValueError("invalid hex digit")
    return bytes(
        (int(text[index], 16) << 4) | int(text[index + 1], 16)
        for index in range(0, len(text), 2)
    )


def balanced_trits(value: int, minimum_width: int = 1) -> list[int]:
    if not isinstance(value, int):
        raise TypeError("value must be an integer")
    out: list[int] = []
    while value:
        remainder = value % 3
        value //= 3
        if remainder == 2:
            remainder, value = -1, value + 1
        out.append(remainder)
    while len(out) < minimum_width:
        out.append(0)
    if not out:
        out.append(0)
    out.reverse()
    return out


def trits_to_int(trits: Iterable[int]) -> int:
    result = 0
    seen = False
    for trit in trits:
        if trit not in (-1, 0, 1):
            raise ValueError("invalid balanced trit")
        seen = True
        result = result * 3 + trit
    return result if seen else 0


def is_trit_literal(value: str) -> bool:
    return (
        len(value) >= 3
        and value[0] == "0"
        and value[1] in "tTyY"
        and all(char in "-0+" for char in value[2:])
    )


def parse_trit_literal(value: str) -> list[int]:
    if not is_trit_literal(value):
        raise ValueError("invalid exact-trit literal")
    return [{"-": -1, "0": 0, "+": 1}[char] for char in value[2:]]


def format_trit_literal(trits: Iterable[int]) -> str:
    values = list(trits)
    if not values:
        values = [0]
    if any(value not in (-1, 0, 1) for value in values):
        raise ValueError("invalid balanced trit")
    return "0t" + "".join("-" if value < 0 else "+" if value > 0 else "0" for value in values)


def _encode_grouped(trits: Iterable[int], alphabet: str, width: int, prefix: str) -> str:
    values = list(trits)
    if not values:
        values = [0]
    if any(value not in (-1, 0, 1) for value in values):
        raise ValueError("invalid balanced trit")
    values = [0] * ((-len(values)) % width) + values
    out: list[str] = []
    for start in range(0, len(values), width):
        digit = 0
        for trit in values[start : start + width]:
            digit = digit * 3 + trit + 1
        out.append(alphabet[digit])
    return prefix + "".join(out)


def _decode_grouped(value: str, alphabet: str, width: int, prefix: str) -> list[int]:
    if not value.startswith(prefix) or len(value) == len(prefix):
        raise ValueError("invalid compact ternary dump")
    out: list[int] = []
    for char in value[len(prefix) :]:
        try:
            digit = alphabet.index(char)
        except ValueError as exc:
            raise ValueError("invalid compact ternary digit") from exc
        group: list[int] = []
        for shift in range(width - 1, -1, -1):
            power = 3**shift
            ordinary, digit = divmod(digit, power)
            group.append(ordinary - 1)
        out.extend(group)
    return out


def _parse_grouped_checked(
    value: str,
    alphabet: str,
    width: int,
    prefix: str,
) -> int | None:
    if not value.startswith(prefix) or len(value) == len(prefix):
        return None
    result = 0
    for char in value[len(prefix) :]:
        try:
            digit = alphabet.index(char)
        except ValueError:
            return None
        for shift in range(width - 1, -1, -1):
            power = 3**shift
            ordinary, digit = divmod(digit, power)
            result = result * 3 + ordinary - 1
            if not INT64_MIN <= result <= INT64_MAX:
                return None
    return result


def format_base27(value: int | Iterable[int]) -> str:
    trits = balanced_trits(value) if isinstance(value, int) else list(value)
    return _encode_grouped(trits, BASE27_ALPHABET, 3, "0z27:")


def format_base81(value: int | Iterable[int]) -> str:
    trits = balanced_trits(value) if isinstance(value, int) else list(value)
    return _encode_grouped(trits, BASE81_ALPHABET, 4, "0z81:")


def parse_base27(value: str) -> list[int]:
    return _decode_grouped(value, BASE27_ALPHABET, 3, "0z27:")


def parse_base81(value: str) -> list[int]:
    return _decode_grouped(value, BASE81_ALPHABET, 4, "0z81:")


def looks_like_numeric_literal(value: str) -> bool:
    if not value:
        return False
    offset = 1 if value[0] in "+-" else 0
    if offset == len(value):
        return False
    return value[offset].isdigit() or (
        len(value) >= offset + 2
        and value[offset] == "0"
        and value[offset + 1] in "tTyYzZ"
    )


def parse_numeric_literal(value: str) -> int | None:
    if is_trit_literal(value):
        result = 0
        for trit in parse_trit_literal(value):
            result = result * 3 + trit
            if not INT64_MIN <= result <= INT64_MAX:
                return None
        return result
    if value.startswith("0z27:"):
        return _parse_grouped_checked(value, BASE27_ALPHABET, 3, "0z27:")
    if value.startswith("0z81:"):
        return _parse_grouped_checked(value, BASE81_ALPHABET, 4, "0z81:")

    negative = value.startswith("-")
    unsigned = value[1:] if value[:1] in "+-" else value
    base = 16 if unsigned.startswith(("0x", "0X")) else 10
    if base == 16:
        unsigned = unsigned[2:]
    if not unsigned:
        return None
    allowed = "0123456789abcdefABCDEF" if base == 16 else "0123456789"
    if any(char not in allowed for char in unsigned):
        return None
    limit = (1 << 63) if negative else INT64_MAX
    magnitude = 0
    for char in unsigned:
        digit = int(char, base)
        if magnitude > (limit - digit) // base:
            return None
        magnitude = magnitude * base + digit
    return -magnitude if negative else magnitude


def format_integer_dump(value: int) -> str:
    return (
        f"hex=0x{value & ((1 << 64) - 1):X} "
        f"trits={format_trit_literal(balanced_trits(value))} "
        f"base27={format_base27(value)} base81={format_base81(value)}"
    )
