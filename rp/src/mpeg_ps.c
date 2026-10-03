/**
 * File: mpeg_ps.c
 * Description: MPEG-1 program stream demultiplexer. See mpeg_ps.h.
 *
 * ISO/IEC 11172-1: a stream of packs (start code 0x000001BA, then the
 * system clock and the mux rate in 8 bytes), an optional system header
 * (0x000001BB and a 16-bit length) and packets (0x000001 + stream id, a
 * 16-bit length, a header of stuffing bytes 0xFF, an optional STD buffer
 * size and optional time stamps, then the payload). 0x000001B9 ends it.
 */

#include "mpeg_ps.h"

#include <string.h>

#define SC_END 0xB9u
#define SC_PACK 0xBAu
#define SC_SYSTEM 0xBBu
#define SC_FIRST_STREAM 0xBCu  // stream ids 0xBC..0xFF carry a length

void mpeg_ps_init_stream(mpeg_ps_t *ps, mpeg_ps_read_fn read, void *ctx,
                         uint8_t stream) {
  memset(ps, 0, sizeof(*ps));
  ps->read = read;
  ps->ctx = ctx;
  ps->stream = stream;
}

void mpeg_ps_init(mpeg_ps_t *ps, mpeg_ps_read_fn read, void *ctx) {
  mpeg_ps_init_stream(ps, read, ctx, MPEG_PS_VIDEO);
}

// Makes at least one byte available; false at the end of the file.
static bool ps_refill(mpeg_ps_t *ps) {
  if (ps->in_pos < ps->in_len) {
    return true;
  }
  if (ps->eof) {
    return false;
  }
  int got = ps->read(ps->ctx, ps->in, MPEG_PS_BUFFER);
  if (got <= 0) {
    ps->error = got < 0;
    ps->eof = true;
    ps->in_pos = ps->in_len = 0;
    return false;
  }
  ps->in_pos = 0;
  ps->in_len = (uint32_t)got;
  return true;
}

static int ps_byte(mpeg_ps_t *ps) {
  if (!ps_refill(ps)) {
    return -1;
  }
  return ps->in[ps->in_pos++];
}

static void ps_skip(mpeg_ps_t *ps, uint32_t n) {
  ps->skipped_bytes += n;
  while (n > 0 && ps_refill(ps)) {
    uint32_t step = ps->in_len - ps->in_pos;
    if (step > n) {
      step = n;
    }
    ps->in_pos += step;
    n -= step;
  }
}

static int ps_u16(mpeg_ps_t *ps) {
  int hi = ps_byte(ps);
  int lo = ps_byte(ps);
  return (hi < 0 || lo < 0) ? -1 : (hi << 8) | lo;
}

// Reads the header of a packet of the stream, `length` bytes, and leaves
// the payload's size in pes_left.
static void ps_packet(mpeg_ps_t *ps, uint32_t length) {
  uint32_t used = 0;
  int b = ps_byte(ps);
  used++;
  while (b == 0xFF && used < length) {  // stuffing
    b = ps_byte(ps);
    used++;
  }
  if (b >= 0 && (b & 0xC0) == 0x40) {  // STD buffer scale and size
    ps_byte(ps);
    b = ps_byte(ps);
    used += 2;
  }
  if (b >= 0 && (b & 0xF0) == 0x20) {  // PTS
    ps_skip(ps, 4);
    used += 4;
  } else if (b >= 0 && (b & 0xF0) == 0x30) {  // PTS and DTS
    ps_skip(ps, 9);
    used += 9;
  } else if (b != 0x0F) {
    // Not an MPEG-1 packet header: drop the packet.
    ps_skip(ps, length > used ? length - used : 0);
    return;
  }
  ps->skipped_bytes += used;
  ps->pes_left = (length > used) ? length - used : 0;
  ps->packets++;
}

// Finds the stream's next payload. False at the end of the stream.
static bool ps_next_packet(mpeg_ps_t *ps) {
  uint32_t shift = 0xFFFFFFu;
  for (;;) {
    int b = ps_byte(ps);
    if (b < 0) {
      return false;
    }
    if ((shift & 0xFFFFFFu) != 0x000001u) {
      shift = (shift << 8) | (uint32_t)b;
      ps->skipped_bytes++;
      continue;
    }
    shift = 0xFFFFFFu;
    if (b == SC_END) {
      ps->eof = true;
      return false;
    }
    if (b == SC_PACK) {
      int first = ps_byte(ps);
      // MPEG-1: 0010 xxxx, 8 bytes in all. MPEG-2: 01xx xxxx, 10 bytes plus
      // the stuffing counted in the last one.
      if (first >= 0 && (first & 0xC0) == 0x40) {
        ps_skip(ps, 8);
        int last = ps_byte(ps);
        ps_skip(ps, last >= 0 ? (uint32_t)(last & 7) : 0);
      } else {
        ps_skip(ps, 7);
      }
      continue;
    }
    if ((uint32_t)b < SC_FIRST_STREAM && b != SC_SYSTEM) {
      continue;  // not a system start code: keep scanning
    }
    int length = ps_u16(ps);
    if (length < 0) {
      return false;
    }
    if (b == ps->stream) {
      ps_packet(ps, (uint32_t)length);
      if (ps->pes_left > 0) {
        return true;
      }
    } else {
      ps_skip(ps, (uint32_t)length);
    }
  }
}

uint32_t mpeg_ps_read(mpeg_ps_t *ps, uint8_t *buf, uint32_t len) {
  uint32_t done = 0;
  while (done < len) {
    if (ps->pes_left == 0 && !ps_next_packet(ps)) {
      break;
    }
    if (!ps_refill(ps)) {
      ps->pes_left = 0;
      break;
    }
    uint32_t step = ps->in_len - ps->in_pos;
    if (step > ps->pes_left) {
      step = ps->pes_left;
    }
    if (step > len - done) {
      step = len - done;
    }
    memcpy(buf + done, ps->in + ps->in_pos, step);
    ps->in_pos += step;
    ps->pes_left -= step;
    done += step;
  }
  ps->bytes += done;
  return done;
}
