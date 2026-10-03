#!/usr/bin/env python3
"""Generates rp/src/include/mpeg1_tables.h: the variable-length code tables of
MPEG-1 video (ISO/IEC 11172-2, Annex B) as lookup tables for the decoder in
rp/src/mpeg1_video.c.

The code lists below are the standard's. They were taken from pl_mpeg
(https://github.com/phoboslab/pl_mpeg, MIT licence, Dominic Szablewski),
whose decoder this firmware's is checked against, and are reproduced here as
plain code strings.

Every table is a direct lookup on the next N bits of the stream, N the
longest code: an entry holds the code's length and its value, 0 for a bit
pattern that is no code. The coefficient table is the exception (codes up to
16 bits): a first table on 8 bits for the codes that start with a non-zero
byte, and a second one on 10 bits for the codes that start with 6 zeros.

    python3 tools/gen_mpeg1_tables.py > rp/src/include/mpeg1_tables.h
"""

ESCAPE = "escape"

MB_ADDRESS_INCREMENT = [
    ("00000001000", 35), ("00000001111", 34), ("00000011000", 33), ("00000011001", 32), ("00000011010", 31), ("00000011011", 30),
    ("00000011100", 29), ("00000011101", 28), ("00000011110", 27), ("00000011111", 26), ("00000100000", 25), ("00000100001", 24),
    ("00000100010", 23), ("00000100011", 22), ("0000010010", 21), ("0000010011", 20), ("0000010100", 19), ("0000010101", 18),
    ("0000010110", 17), ("0000010111", 16), ("00000110", 15), ("00000111", 14), ("00001000", 13), ("00001001", 12),
    ("00001010", 11), ("00001011", 10), ("0000110", 9), ("0000111", 8), ("00010", 7), ("00011", 6),
    ("0010", 5), ("0011", 4), ("010", 3), ("011", 2), ("1", 1),
]
MB_TYPE_I = [
    ("01", 17), ("1", 1),
]
MB_TYPE_P = [
    ("000001", 17), ("00001", 18), ("00010", 26), ("00011", 1), ("001", 8), ("01", 2),
    ("1", 10),
]
MB_TYPE_B = [
    ("000001", 17), ("000010", 22), ("000011", 26), ("00010", 30), ("00011", 1), ("0010", 8),
    ("0011", 10), ("010", 4), ("011", 6), ("10", 12), ("11", 14),
]
CODED_BLOCK_PATTERN = [
    ("000000010", 39), ("000000011", 27), ("000000100", 59), ("000000101", 55), ("000000110", 47), ("000000111", 31),
    ("00000100", 58), ("00000101", 54), ("00000110", 46), ("00000111", 30), ("00001000", 57), ("00001001", 53),
    ("00001010", 45), ("00001011", 29), ("00001100", 38), ("00001101", 26), ("00001110", 37), ("00001111", 25),
    ("00010000", 43), ("00010001", 23), ("00010010", 51), ("00010011", 15), ("00010100", 42), ("00010101", 22),
    ("00010110", 50), ("00010111", 14), ("00011000", 41), ("00011001", 21), ("00011010", 49), ("00011011", 13),
    ("00011100", 35), ("00011101", 19), ("00011110", 11), ("00011111", 7), ("0010000", 34), ("0010001", 18),
    ("0010010", 10), ("0010011", 6), ("0010100", 33), ("0010101", 17), ("0010110", 9), ("0010111", 5),
    ("001100", 63), ("001101", 3), ("001110", 36), ("001111", 24), ("01000", 62), ("01001", 2),
    ("01010", 61), ("01011", 1), ("01100", 56), ("01101", 52), ("01110", 44), ("01111", 28),
    ("10000", 40), ("10001", 20), ("10010", 48), ("10011", 12), ("1010", 32), ("1011", 16),
    ("1100", 8), ("1101", 4), ("111", 60),
]
MOTION_CODE = [
    ("00000011000", 16), ("00000011001", -16), ("00000011010", 15), ("00000011011", -15), ("00000011100", 14), ("00000011101", -14),
    ("00000011110", 13), ("00000011111", -13), ("00000100000", 12), ("00000100001", -12), ("00000100010", 11), ("00000100011", -11),
    ("0000010010", 10), ("0000010011", -10), ("0000010100", 9), ("0000010101", -9), ("0000010110", 8), ("0000010111", -8),
    ("00000110", 7), ("00000111", -7), ("00001000", 6), ("00001001", -6), ("00001010", 5), ("00001011", -5),
    ("0000110", 4), ("0000111", -4), ("00010", 3), ("00011", -3), ("0010", 2), ("0011", -2),
    ("010", 1), ("011", -1), ("1", 0),
]
DC_SIZE_LUMINANCE = [
    ("00", 1), ("01", 2), ("100", 0), ("101", 3), ("110", 4), ("1110", 5),
    ("11110", 6), ("111110", 7), ("1111110", 8),
]
DC_SIZE_CHROMINANCE = [
    ("00", 0), ("01", 1), ("10", 2), ("110", 3), ("1110", 4), ("11110", 5),
    ("111110", 6), ("1111110", 7), ("11111110", 8),
]
DCT_COEFF = [
    ("0000000000010000", 1, 18), ("0000000000010001", 1, 17), ("0000000000010010", 1, 16), ("0000000000010011", 1, 15),
    ("0000000000010100", 6, 3), ("0000000000010101", 16, 2), ("0000000000010110", 15, 2), ("0000000000010111", 14, 2),
    ("0000000000011000", 13, 2), ("0000000000011001", 12, 2), ("0000000000011010", 11, 2), ("0000000000011011", 31, 1),
    ("0000000000011100", 30, 1), ("0000000000011101", 29, 1), ("0000000000011110", 28, 1), ("0000000000011111", 27, 1),
    ("000000000010000", 0, 40), ("000000000010001", 0, 39), ("000000000010010", 0, 38), ("000000000010011", 0, 37),
    ("000000000010100", 0, 36), ("000000000010101", 0, 35), ("000000000010110", 0, 34), ("000000000010111", 0, 33),
    ("000000000011000", 0, 32), ("000000000011001", 1, 14), ("000000000011010", 1, 13), ("000000000011011", 1, 12),
    ("000000000011100", 1, 11), ("000000000011101", 1, 10), ("000000000011110", 1, 9), ("000000000011111", 1, 8),
    ("00000000010000", 0, 31), ("00000000010001", 0, 30), ("00000000010010", 0, 29), ("00000000010011", 0, 28),
    ("00000000010100", 0, 27), ("00000000010101", 0, 26), ("00000000010110", 0, 25), ("00000000010111", 0, 24),
    ("00000000011000", 0, 23), ("00000000011001", 0, 22), ("00000000011010", 0, 21), ("00000000011011", 0, 20),
    ("00000000011100", 0, 19), ("00000000011101", 0, 18), ("00000000011110", 0, 17), ("00000000011111", 0, 16),
    ("0000000010000", 10, 2), ("0000000010001", 9, 2), ("0000000010010", 5, 3), ("0000000010011", 3, 4),
    ("0000000010100", 2, 5), ("0000000010101", 1, 7), ("0000000010110", 1, 6), ("0000000010111", 0, 15),
    ("0000000011000", 0, 14), ("0000000011001", 0, 13), ("0000000011010", 0, 12), ("0000000011011", 26, 1),
    ("0000000011100", 25, 1), ("0000000011101", 24, 1), ("0000000011110", 23, 1), ("0000000011111", 22, 1),
    ("000000010000", 0, 11), ("000000010001", 8, 2), ("000000010010", 4, 3), ("000000010011", 0, 10),
    ("000000010100", 2, 4), ("000000010101", 7, 2), ("000000010110", 21, 1), ("000000010111", 20, 1),
    ("000000011000", 0, 9), ("000000011001", 19, 1), ("000000011010", 18, 1), ("000000011011", 1, 5),
    ("000000011100", 3, 3), ("000000011101", 0, 8), ("000000011110", 6, 2), ("000000011111", 17, 1),
    ("0000001000", 16, 1), ("0000001001", 5, 2), ("0000001010", 0, 7), ("0000001011", 2, 3),
    ("0000001100", 1, 4), ("0000001101", 15, 1), ("0000001110", 14, 1), ("0000001111", 4, 2),
    ("000001", ESCAPE), ("0000100", 2, 2), ("0000101", 9, 1), ("0000110", 0, 4),
    ("0000111", 8, 1), ("000100", 7, 1), ("000101", 6, 1), ("000110", 1, 2),
    ("000111", 5, 1), ("00100000", 13, 1), ("00100001", 0, 6), ("00100010", 12, 1),
    ("00100011", 11, 1), ("00100100", 3, 2), ("00100101", 1, 3), ("00100110", 0, 5),
    ("00100111", 10, 1), ("00101", 0, 3), ("00110", 4, 1), ("00111", 3, 1),
    ("0100", 0, 2), ("0101", 2, 1), ("011", 1, 1), ("1", 0, 1),
]


def direct(name, codes, bits, value_of=lambda v: v):
    """uint16 entries: length << 8 | value (0..255)."""
    table = [0] * (1 << bits)
    for code, value in codes:
        n = len(code)
        first = int(code, 2) << (bits - n)
        v = value_of(value)
        assert 0 <= v < 256 and n < 256
        for i in range(first, first + (1 << (bits - n))):
            assert table[i] == 0, (name, code)
            table[i] = (n << 8) | v
    return emit(name, table, bits)


def emit(name, table, bits):
    lines = [f"// {len(table)} entries, indexed by the next {bits} bits."]
    lines.append(f"static const uint16_t {name}[{len(table)}] = {{")
    for i in range(0, len(table), 8):
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in table[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def dct_entry(code, rest):
    """uint16: length << 11 | run << 6 | level. Escape: level 0, run 0."""
    if rest == (ESCAPE,):
        return len(code) << 11
    run, level = rest
    assert 0 < level < 64 and run < 32 and len(code) < 32
    return (len(code) << 11) | (run << 6) | level


def dct_tables():
    first = [0] * 256  # codes whose first byte is not 0..3
    second = [0] * 1024  # codes that start with 000000: index = next 10 bits
    for item in DCT_COEFF:
        code, rest = item[0], item[1:]
        entry = dct_entry(code, rest)
        if code.startswith("000000"):
            assert 10 <= len(code) <= 16
            tail = code[6:]
            idx = int(tail, 2) << (10 - len(tail))
            for i in range(idx, idx + (1 << (10 - len(tail)))):
                assert second[i] == 0
                second[i] = entry
        else:
            assert len(code) <= 8
            idx = int(code, 2) << (8 - len(code))
            for i in range(idx, idx + (1 << (8 - len(code)))):
                assert first[i] == 0
                first[i] = entry
    return (emit("mpeg1_dct_first8", first, 8) + "\n\n" +
            emit("mpeg1_dct_zeros6", second, 10))


def main():
    print("// Generated by tools/gen_mpeg1_tables.py: do not edit.")
    print("//")
    print("// MPEG-1 video variable-length codes (ISO/IEC 11172-2 Annex B) as")
    print("// lookup tables. Include from one .c file only (mpeg1_video.c).")
    print("")
    print("#ifndef MPEG1_TABLES_H")
    print("#define MPEG1_TABLES_H")
    print("")
    print("#include <stdint.h>")
    print("")
    print("// Address increments 1..33; 34 is stuffing, 35 the escape (+33).")
    print(direct("mpeg1_mb_address_increment", MB_ADDRESS_INCREMENT, 11))
    print("")
    print("// Macroblock types: bit 0 intra, 1 coded pattern, 2 motion backward,")
    print("// 3 motion forward, 4 quantiser scale follows.")
    print(direct("mpeg1_mb_type_i", MB_TYPE_I, 2))
    print("")
    print(direct("mpeg1_mb_type_p", MB_TYPE_P, 6))
    print("")
    print(direct("mpeg1_mb_type_b", MB_TYPE_B, 6))
    print("")
    print(direct("mpeg1_coded_block_pattern", CODED_BLOCK_PATTERN, 9))
    print("")
    print("// Motion codes -16..16, stored + 16.")
    print(direct("mpeg1_motion_code", MOTION_CODE, 11, lambda v: v + 16))
    print("")
    print(direct("mpeg1_dc_size_luminance", DC_SIZE_LUMINANCE, 7))
    print("")
    print(direct("mpeg1_dc_size_chrominance", DC_SIZE_CHROMINANCE, 8))
    print("")
    print("// Coefficients: length << 11 | run << 6 | level, level 0 is the escape")
    print("// (6 bits). The code \"1\" (run 0, level 1) is dct_coeff_first's; in")
    print("// dct_coeff_next the decoder reads \"10\" as end of block and \"11\" as")
    print("// run 0, level 1 itself. Codes starting with 6 zeros are in the second")
    print("// table, indexed by the 10 bits after them.")
    print(dct_tables())
    print("")
    print("#endif  // MPEG1_TABLES_H")


if __name__ == "__main__":
    main()
