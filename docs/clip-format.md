# The clip file, version 1

A converted clip of the game: what the cartridge writes to the SD card from one MPEG clip of the
CD-ROM image, and what the player reads back. One record per output frame, at 25 frames a
second: the frame's sound, and its picture when a new one starts, stored alone or as the changes
from the picture before. A clip is written and read in one pass, front to back; only the header
is written again, once, when the clip is finished.

The reference code is `rp/src/clip.c` (`clip.h`): the encoder, the decoder and the writer, the
same on the cartridge and in `tools/dlconv` (`dlconv encode`, `dlconv play`). `tests/host/test_clip.c`
checks it.

## Conventions

- Integers are little-endian.
- A picture is 320 x 200 colour indices, 0 to 15, one row of 320 after another.
- A palette is 16 words `0x0RGB`, 4 bits a gun. The converter makes entry 0 of every palette
  black: on an ST it is also the border's colour. A clip converted for an ST (3 bits a gun) stores
  each gun's level shifted up: `gun = level << 1`, so the ST's own value is `gun >> 1`.
- Sound is signed 8-bit samples, mono, 22,050 a second: 882 a frame.

## Header (64 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `DLCL` |
| 4 | 2 | version: 1 |
| 6 | 2 | header size: 64 |
| 8 | 4 | frames: the records that follow |
| 12 | 2 | frames a second: 25 |
| 14 | 2 | samples a second: 22,050 |
| 16 | 2 | samples a frame: 882 |
| 18 | 1 | the palette's depth: 3 (an ST) or 4 (an STE) |
| 19 | 1 | flags: bit 0, mirrored, is reserved and 0 |
| 20 | 2 | the converter's version (`CONVERT_VERSION`) |
| 22 | 2 | the converter's palette stability, in % (`0xFFFF`: none) |
| 24 | 4 | the source clip's size in bytes |
| 28 | 4 | the source clip's CRC-32 |
| 32 | 4 | the key index's offset from the file's start |
| 36 | 4 | the key index's entries |
| 40 | 4 | the largest record, in bytes |
| 44 | 4 | CRC-32 of everything after the header: the records and the index |
| 48 | 16 | reserved, 0 |

A clip whose converter version, depth or source (size and CRC-32) differs from what the cartridge
would produce now is converted again; two sources with the same size and CRC-32 are the same clip.
The CRC-32 is zlib's.

## Records

Records follow the header, one per frame, in order. A record has no length field: it ends where
its parts do.

| Part | Size | When |
|---|---|---|
| kind | 1 | always |
| palette | 32 | when bit 7 of the kind is set |
| sound | 882 | always: the frame's samples |
| picture | 200 rows | when the kind is a delta or a key |

The kind's low two bits:

- `0` held: the picture before, shown again.
- `1` delta: a new picture, each row stored against the same row of the picture before.
- `2` key: a new picture, each row stored alone. Playing can start here.
- `3` is reserved.

Bit 7 says a palette follows, which then applies from this frame on. The other bits are 0. A
key's record always carries its palette, so that playing can start there; otherwise the palette
is written only when it changed.

The largest record is 1 + 32 + 882 + 200 x 165 = 33,915 bytes: a reader holding that many bytes
always holds a whole record.

## Rows

A row is a sequence of one-byte opcodes that together cover its 320 pixels exactly. The two high
bits are the operation, the six low bits a count less one (1 to 64):

| Opcode | Operation |
|---|---|
| `00nnnnnn` | skip n + 1 pixels: they keep the picture before's colours (a delta only) |
| `01nnnnnn` | n + 1 literal pixels follow, two a byte, the first in the high nibble; an odd count leaves the last byte's low nibble 0 |
| `10nnnnnn` | repeat one colour n + 1 times: the next byte's low nibble is the colour |
| `11000000` | the rest of the row keeps the picture before's colours (a delta only) |

The other `11` opcodes are reserved. An operation that would pass the row's end makes the record
malformed.

The encoder is greedy: an unchanged run is skipped (to the row's end in one byte), a colour
repeated three times or more is a repeat, anything else is literal until a skip of two or a
repeat of three could start. A row whose encoding would be longer than 165 bytes is stored as
five literals of 64 instead (5 x 33 = 165 bytes), so a row never takes more than its pixels do.

## Key pictures and the index

The index lists the clip's first picture, then any picture that starts 50 frames (2 s) or more
after the last one listed, and any picture the converter is asked to list (the points the game
will start a clip from); each of them is a key. Other pictures may be keys too: the cartridge
stores every picture whole, since a delta needs the picture before and the cartridge has no room
to keep it (keeping it on the card took longer than the conversion itself). `dlconv encode`
writes the same file, or deltas with `--deltas`.

After the records, the index lists its keys: each one's frame (4 bytes) and its record's offset
from the file's start (4 bytes). The index ends the file.

## Reading

A player keeps the picture shown (320 x 200 indices) and the palette, and for each record in
turn:

1. reads the kind; when bit 7 is set, takes the palette that follows;
2. hands the 882 samples to the sound;
3. for a delta, applies each row to the picture it keeps; for a key, writes every pixel; for a
   held frame, keeps the picture;
4. shows the picture with the palette for the frame.

To start in the middle of a clip, a player finds the last key at or before the frame it wants in
the index and reads from that record on.

## The game, converted

All 194 clips of the game, as the cartridge converts them (palette stability 10 %, the mixing
dither, every picture whole): 30,915 frames from 18,436 pictures, 691 of them in the index;
534.5 MB for an STE and 546.4 MB for an ST, of which 27.3 MB is sound. A picture's record takes
about 28.4 KB on average for an STE, 29.0 KB for an ST (its sound included). As deltas
(`dlconv encode --deltas`) the game would take 338.5 MB and 342.4 MB, 17.8 KB a picture's
record. A clip's sound starts with its pictures; where the source's sound ends before them (a
few frames in most clips), the frames are silent.
