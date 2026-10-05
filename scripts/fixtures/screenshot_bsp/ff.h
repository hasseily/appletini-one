#ifndef SCREENSHOT_TEST_FF_H
#define SCREENSHOT_TEST_FF_H
#include <stdint.h>
#include <stddef.h>
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef size_t FSIZE_t;
typedef struct { int unused; } FATFS;
typedef struct { int index, generation, open; size_t position, size; } FIL;
typedef enum {
    FR_OK, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED
} FRESULT;
#define FA_WRITE 2U
#define FA_CREATE_NEW 4U
#define f_size(fp) ((fp)->size)
#define f_tell(fp) ((fp)->position)
FRESULT f_mount(FATFS *, const char *, uint8_t);
FRESULT f_mkdir(const char *);
FRESULT f_open(FIL *, const char *, uint8_t);
FRESULT f_write(FIL *, const void *, UINT, UINT *);
FRESULT f_lseek(FIL *, FSIZE_t);
FRESULT f_close(FIL *);
FRESULT f_rename(const char *, const char *);
FRESULT f_unlink(const char *);
#endif
