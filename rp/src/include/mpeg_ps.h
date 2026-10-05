/**
 * File: mpeg_ps.h
 * Description: MPEG-1 program stream demultiplexer: one elementary stream
 *              of a .MPG file, read through a small buffer.
 *
 * The file is read through a function the caller gives (an iso9660_file_t
 * on the RP, a FILE on a PC), 2 KB at a time. Pack and system headers,
 * padding and every other stream are skipped; the payload of the chosen
 * stream (the first video stream, 0xE0, or the first audio stream, 0xC0)
 * is handed out in order, its PES headers removed. A clip's picture and
 * sound are read by two demultiplexers over the same file.
 * Plain C, no allocation, no SDK: it builds for the firmware and for host
 * tests.
 */

#ifndef MPEG_PS_H
#define MPEG_PS_H

#include <stdbool.h>
#include <stdint.h>

#define MPEG_PS_BUFFER 2048u
#define MPEG_PS_VIDEO 0xE0u  // stream ids
#define MPEG_PS_AUDIO 0xC0u

// Reads up to `len` bytes of the file into `buf`. Returns the number read,
// 0 at the end of the file, a negative value on error.
typedef int (*mpeg_ps_read_fn)(void *ctx, uint8_t *buf, uint32_t len);

typedef struct {
  mpeg_ps_read_fn read;
  void *ctx;
  uint8_t in[MPEG_PS_BUFFER];
  uint32_t in_pos;
  uint32_t in_len;
  bool eof;           // the file is exhausted (or the end code was read)
  bool error;         // the read function failed
  uint8_t stream;     // the stream id handed out
  uint32_t pes_left;  // payload bytes left in the current packet
  uint32_t packets;   // of the stream
  uint32_t bytes;     // of its payload
  uint32_t skipped_bytes;  // in packets of other streams, padding, headers
} mpeg_ps_t;

// The video stream (MPEG_PS_VIDEO).
void mpeg_ps_init(mpeg_ps_t *ps, mpeg_ps_read_fn read, void *ctx);

// The stream `stream` (MPEG_PS_VIDEO, MPEG_PS_AUDIO).
void mpeg_ps_init_stream(mpeg_ps_t *ps, mpeg_ps_read_fn read, void *ctx,
                         uint8_t stream);

// Copies up to `len` bytes of the elementary stream into `buf`. Returns the
// number copied; 0 only at the end of the stream.
uint32_t mpeg_ps_read(mpeg_ps_t *ps, uint8_t *buf, uint32_t len);

#endif  // MPEG_PS_H
