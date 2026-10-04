/* FatFs's calls for the converter in a browser or in Node (dlweb.c): the
 * CD-ROM image is read and the clip file written through the JavaScript
 * host. Only what iso9660.c and convjob.c use. */
#ifndef DLWEB_FF_H
#define DLWEB_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef uint64_t FSIZE_t;  // as FatFs with exFAT, the cartridge's
typedef unsigned char BYTE;
typedef enum {
  FR_OK = 0,
  FR_DISK_ERR = 1,
  FR_NO_FILE = 4,
  FR_NO_PATH = 5,
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08

typedef struct {
  int writing;     // the clip file; otherwise the image
  FSIZE_t fptr;
  FSIZE_t size;
} FIL;

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_close(FIL *fp);
#define f_size(fp) ((fp)->size)
#define f_tell(fp) ((fp)->fptr)

#endif
