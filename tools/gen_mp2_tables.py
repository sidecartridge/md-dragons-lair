#!/usr/bin/env python3
"""Generates rp/src/include/mp2_tables.h: the tables of the MPEG-1 Layer II
audio decoder in rp/src/mp2_audio.c (ISO/IEC 11172-3).

The allocation tables (3-B.2a to 3-B.2d, as lookups) and the synthesis
window (3-B.3) are the standard's. They were taken from pl_mpeg
(https://github.com/phoboslab/pl_mpeg, MIT licence, Dominic Szablewski) and
are reproduced here: the window as D[i] x 65536, which is exact (every
coefficient is a multiple of 2^-16).

The decoder synthesises only the lower 16 subbands and only every other
output sample, so its output is the sound low-passed at a quarter of the
sampling rate and decimated by 2 (22,050 Hz from 44,100), in a quarter of
the work. Of the standard's 64 matrixed values V[i] a time slot needs
V[2m] and V[32 + 2m] (m < 16), and by the symmetries of the cosines only 16
of those are distinct: A[m] = V[2m] (m < 8) and B[m] = V[32 + 2m]
(0 < m <= 8). The window keeps its even-indexed half. Everything is
computed here and checked, so the firmware only adds and multiplies
integers.

    python3 tools/gen_mp2_tables.py > rp/src/include/mp2_tables.h
"""

import math
import random

# D[i] x 65536 (Table 3-B.3).
WINDOW_X65536 = [
    0, -1, -1, -1, -1, -1, -1, -2,
    -2, -2, -2, -3, -3, -4, -4, -5,
    -5, -6, -7, -7, -8, -9, -10, -11,
    -13, -14, -16, -17, -19, -21, -24, -26,
    -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154,
    -161, -169, -176, -183, -190, -196, -202, -208,
    213, 218, 222, 225, 227, 228, 228, 227,
    224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36,
    -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919,
    -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
    -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
    2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535,
    1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006,
    -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597,
    -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
    -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750,
    -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082,
    70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
    -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189,
    -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137,
    -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420,
    -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038, 74992, 74856, 74630, 74313, 73908, 73415, 72835,
    72169, 71420, 70590, 69679, 68692, 67629, 66494, 65290,
    64019, 62684, 61289, 59838, 58333, 56778, 55178, 53534,
    51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336,
    37489, 35640, 33791, 31947, 30112, 28289, 26482, 24694,
    22929, 21189, 19478, 17799, 16155, 14548, 12980, 11455,
    9975, 8540, 7154, 5818, 4533, 3300, 2122, 998,
    -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959,
    6574, 7134, 7640, 8092, 8492, 8840, 9139, 9389,
    9592, 9750, 9863, 9935, 9966, 9959, 9916, 9838,
    9727, 9585, 9416, 9219, 8998, 8755, 8491, 8209,
    7910, 7597, 7271, 6935, 6589, 6237, 5879, 5517,
    5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663,
    2330, 2006, 1692, 1388, 1095, 814, 545, 288,
    45, -185, -402, -605, -794, -970, -1131, -1280,
    -1414, -1535, -1644, -1739, -1822, -1893, -1952, -2000,
    2037, 2063, 2080, 2087, 2085, 2075, 2057, 2032,
    2001, 1962, 1919, 1870, 1817, 1759, 1698, 1634,
    1567, 1498, 1428, 1356, 1283, 1210, 1137, 1064,
    991, 919, 848, 779, 711, 645, 581, 519,
    459, 401, 347, 294, 244, 197, 153, 111,
    72, 36, 2, -29, -57, -83, -106, -127,
    -146, -163, -177, -189, -200, -208, -215, -221,
    -224, -227, -228, -228, -227, -225, -222, -218,
    213, 208, 202, 196, 190, 183, 176, 169,
    161, 154, 147, 139, 132, 125, 117, 111,
    104, 97, 91, 85, 79, 73, 68, 63,
    58, 53, 49, 45, 41, 38, 35, 31,
    29, 26, 24, 21, 19, 17, 16, 14,
    13, 11, 10, 9, 8, 7, 7, 6,
    5, 5, 4, 4, 3, 3, 2, 2,
    2, 2, 1, 1, 1, 1, 1, 1,

]

# Bit rates (kbit/s) of bit rate indices 1..14, sampling rates of 0..2.
BIT_RATES = [32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384]
SAMPLE_RATES = [44100, 48000, 32000]

# Allocation table lookups (pl_mpeg's), in four steps:
# 1. channels (mono, stereo) and bit rate index - 1 -> bit rate class;
# 2. bit rate class and sampling rate -> table and sblimit
#    (high-rate tables: sblimit | 64);
# 3. table and subband -> bits of the allocation (high nibble) and row;
# 4. row and allocation -> quantisation class (1..17; 0: no samples).
STEP_1 = [
    [0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2],  # mono
    [0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2],  # stereo: per channel
]
TAB_A = 27 | 64  # 3-B.2a
TAB_B = 30 | 64  # 3-B.2b
TAB_C = 8        # 3-B.2c
TAB_D = 12       # 3-B.2d
STEP_2 = [
    [TAB_C, TAB_C, TAB_D],  # 32 - 48 kbit/s a channel
    [TAB_A, TAB_A, TAB_A],  # 56 - 80
    [TAB_B, TAB_A, TAB_B],  # 96 and more
]
STEP_3 = [
    [0x44, 0x44] + [0x34] * 10,                              # low rate
    [0x43] * 3 + [0x42] * 8 + [0x31] * 12 + [0x20] * 7,      # high rate
]
STEP_4 = [
    [0, 1, 2, 17],
    [0, 1, 2, 3, 4, 5, 6, 17],
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17],
    [0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17],
    [0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17],
]
# Quantisation classes 1..17 (Table 3-B.4): levels, grouped, bits a code.
QUANT = [
    (3, 1, 5), (5, 1, 7), (7, 0, 3), (9, 1, 10), (15, 0, 4), (31, 0, 5),
    (63, 0, 6), (127, 0, 7), (255, 0, 8), (511, 0, 9), (1023, 0, 10),
    (2047, 0, 11), (4095, 0, 12), (8191, 0, 13), (16383, 0, 14),
    (32767, 0, 15), (65535, 0, 16),
]

Q14 = 1 << 14


def round_half_away(x):
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def n(i, k):
    """The standard's matrixing coefficient N[i][k]."""
    return math.cos((16 + i) * (2 * k + 1) * math.pi / 64)


def check_symmetries():
    for _ in range(20):
        s = [random.uniform(-1, 1) for _ in range(16)]
        v = [sum(s[k] * n(i, k) for k in range(16)) for i in range(64)]
        a = [v[2 * m] for m in range(16)]
        b = [v[32 + 2 * m] for m in range(16)]
        assert abs(a[8]) < 1e-9
        for m in range(1, 8):
            assert abs(a[16 - m] + a[m]) < 1e-9
            assert abs(b[16 - m] - b[m]) < 1e-9
        assert abs(b[0] + a[0]) < 1e-9


def table(name, ctype, rows):
    print(f"static const {ctype} {name}[{len(rows)}][{len(rows[0])}] = {{")
    for r in rows:
        print("    {" + ", ".join(str(x) for x in r) + "},")
    print("};")


def main():
    assert len(WINDOW_X65536) == 512
    check_symmetries()
    print("// Generated by tools/gen_mp2_tables.py: do not edit.")
    print("// MPEG-1 Layer II tables for rp/src/mp2_audio.c; include from there only.")
    print("")
    print("#ifndef MP2_TABLES_H")
    print("#define MP2_TABLES_H")
    print("")
    print("#include <stdint.h>")
    print("")
    print("static const uint16_t mp2_bit_rates[14] = {" +
          ", ".join(str(r) for r in BIT_RATES) + "};")
    print("static const uint16_t mp2_sample_rates[3] = {" +
          ", ".join(str(r) for r in SAMPLE_RATES) + "};")
    print("")
    table("mp2_step_1", "uint8_t", STEP_1)
    table("mp2_step_2", "uint8_t", STEP_2)
    print("static const uint8_t mp2_step_3[2][30] = {")
    for r in STEP_3:
        print("    {" + ", ".join("0x%02X" % x for x in (r + [0] * 30)[:30]) + "},")
    print("};")
    print("static const uint8_t mp2_step_4[5][16] = {")
    for r in STEP_4:
        print("    {" + ", ".join(str(x) for x in r) + "},")
    print("};")
    print("")
    print("// Quantisation classes 1..17: levels, grouped (3 samples a code), bits")
    print("// a code, and round(2^30 / levels), the dequantiser's multiplier.")
    print("typedef struct {")
    print("  uint16_t levels;")
    print("  uint8_t group;")
    print("  uint8_t bits;")
    print("  int32_t mul;")
    print("} mp2_quant_t;")
    print("")
    print("static const mp2_quant_t mp2_quant[17] = {")
    for lv, g, bits in QUANT:
        print(f"    {{{lv}, {g}, {bits}, {round_half_away(2**30 / lv)}}},")
    print("};")
    print("")
    print("// 2^(1 - i/3) for i = 0, 1, 2 (scale factor i, then >> i / 3), Q14.")
    print("static const int32_t mp2_scale_base[3] = {" + ", ".join(
        str(round_half_away(2 ** (1 - i / 3) * Q14)) for i in range(3)) + "};")
    print("")
    print("// Matrixing, Q14: A[m] = sum of S[k] x mp2_cos_a[m][k] (m < 8),")
    print("// B[m] = sum of S[k] x mp2_cos_b[m - 1][k] (0 < m <= 8).")
    table("mp2_cos_a", "int16_t",
          [[round_half_away(n(2 * m, k) * Q14) for k in range(16)]
           for m in range(8)])
    table("mp2_cos_b", "int16_t",
          [[round_half_away(n(32 + 2 * m, k) * Q14) for k in range(16)]
           for m in range(1, 9)])
    print("")
    print("// The window's even half, Q14: output 2m of a time slot is the sum over")
    print("// j < 8 of A(slot - 2j)[m] x mp2_window_a[j][m] and")
    print("// B(slot - 2j - 1)[m] x mp2_window_b[j][m].")
    table("mp2_window_a", "int16_t",
          [[round_half_away(WINDOW_X65536[64 * j + 2 * m] / 4)
            for m in range(16)] for j in range(8)])
    table("mp2_window_b", "int16_t",
          [[round_half_away(WINDOW_X65536[64 * j + 32 + 2 * m] / 4)
            for m in range(16)] for j in range(8)])
    print("")
    print("#endif  // MP2_TABLES_H")


if __name__ == "__main__":
    main()
