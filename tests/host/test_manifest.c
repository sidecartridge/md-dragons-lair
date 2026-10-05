// units: rp/src/manifest.c rp/src/clip.c rp/src/crc32.c
/* A set's manifest: the header and the entries written and read back the
 * same; another file or version refused; an entry with no name or one
 * that does not end refused; a clip file matched against its entry and its
 * set (converter, stability, depth, source, the file's size and CRC-32,
 * the index ending the file), and a change to any of them not matched; a
 * name too long for the entry cut, never written past it. */

#include <string.h>

#include "crc32.h"
#include "manifest.h"
#include "test.h"

static clip_header_t a_clip(void) {
  clip_header_t c;
  memset(&c, 0, sizeof(c));
  c.frames = 1000;
  c.gun_bits = 4;
  c.converter = 2;
  c.keep_percent = 10;
  c.source_bytes = 5000000;
  c.source_crc = 0x0BADC0DEu;
  c.index_offset = 18000000;
  c.index_count = 20;
  c.largest_record = 30000;
  c.crc = 0xC0FFEE11u;
  return c;
}

#define FILE_BYTES (18000000u + 8u * 20u)

int main(void) {
  // The header both ways.
  manifest_header_t h = {194, 4, 2, 10, 0x12345678u};
  uint8_t hb[MANIFEST_HEADER_BYTES];
  manifest_header_write(&h, hb);
  CHECK(memcmp(hb, "DLMF", 4) == 0);
  manifest_header_t h2;
  CHECK_EQ(manifest_header_read(&h2, hb), 0);
  CHECK_EQ(h2.count, 194);
  CHECK_EQ(h2.gun_bits, 4);
  CHECK_EQ(h2.converter, 2);
  CHECK_EQ(h2.keep_percent, 10);
  CHECK_EQ(h2.entries_crc, 0x12345678u);
  uint8_t bad[MANIFEST_HEADER_BYTES];
  memcpy(bad, hb, sizeof(bad));
  bad[0] = 'X';
  CHECK_EQ(manifest_header_read(&h2, bad), -1);
  memcpy(bad, hb, sizeof(bad));
  bad[4] = 2;  // version 2
  CHECK_EQ(manifest_header_read(&h2, bad), -2);
  memcpy(bad, hb, sizeof(bad));
  bad[8] = 64;  // another entry size
  CHECK_EQ(manifest_header_read(&h2, bad), -2);

  // An entry of a clip file, both ways.
  clip_header_t clip = a_clip();
  manifest_entry_t e;
  manifest_entry_of(&e, "S01.MPG", &clip, FILE_BYTES);
  uint8_t eb[MANIFEST_ENTRY_BYTES];
  manifest_entry_write(&e, eb);
  manifest_entry_t e2;
  CHECK_EQ(manifest_entry_read(&e2, eb), 0);
  CHECK(strcmp(e2.name, "S01.MPG") == 0);
  CHECK_EQ(e2.source_bytes, clip.source_bytes);
  CHECK_EQ(e2.source_crc, clip.source_crc);
  CHECK_EQ(e2.file_bytes, FILE_BYTES);
  CHECK_EQ(e2.file_crc, clip.crc);
  memcpy(bad, eb, sizeof(eb));
  bad[0] = '\0';
  CHECK_EQ(manifest_entry_read(&e2, bad), -1);
  memcpy(bad, eb, sizeof(eb));
  memset(bad, 'S', MANIFEST_NAME_BYTES);
  CHECK_EQ(manifest_entry_read(&e2, bad), -1);

  // A name too long: cut to fit, the entry's other fields untouched.
  manifest_entry_of(&e2, "S01234567890123456789.MPG", &clip, FILE_BYTES);
  CHECK_EQ(strlen(e2.name), MANIFEST_NAME_BYTES - 1u);
  manifest_entry_write(&e2, eb);
  CHECK_EQ(eb[MANIFEST_NAME_BYTES - 1u], 0);
  CHECK_EQ(manifest_entry_read(&e2, eb), 0);

  // The clip file matched against its entry and its set.
  manifest_header_t set = {1, 4, 2, 10, 0};
  manifest_entry_of(&e, "S01.MPG", &clip, FILE_BYTES);
  CHECK(manifest_matches(&set, &e, &clip, FILE_BYTES));
  CHECK(!manifest_matches(&set, &e, &clip, FILE_BYTES - 1u));
  clip_header_t c = clip;
  c.converter = 1;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.keep_percent = 0xFFFF;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.gun_bits = 3;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.source_bytes++;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.source_crc ^= 1u;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.crc ^= 1u;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.index_count = 19;  // the index would not end the file
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));
  c = clip;
  c.frames = 0;
  CHECK(!manifest_matches(&set, &e, &c, FILE_BYTES));

  // The entries' CRC-32: what a reader recomputes over the entries read.
  uint8_t entries[3 * MANIFEST_ENTRY_BYTES];
  const char *names[3] = {"S01.MPG", "S01B.MPG", "S38D2.MPG"};
  uint32_t crc = 0;
  for (int i = 0; i < 3; i++) {
    manifest_entry_of(&e, names[i], &clip, FILE_BYTES + (uint32_t)i);
    manifest_entry_write(&e, entries + i * MANIFEST_ENTRY_BYTES);
    crc = crc32_update(crc, entries + i * MANIFEST_ENTRY_BYTES,
                       MANIFEST_ENTRY_BYTES);
  }
  CHECK_EQ(crc, crc32_update(0, entries, sizeof(entries)));
  for (int i = 0; i < 3; i++) {
    CHECK_EQ(manifest_entry_read(&e, entries + i * MANIFEST_ENTRY_BYTES), 0);
    CHECK(strcmp(e.name, names[i]) == 0);
    CHECK_EQ(e.file_bytes, FILE_BYTES + (uint32_t)i);
  }
  TEST_END();
}
