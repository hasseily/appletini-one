/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef FF_H
#define FF_H
#include <stdint.h>
typedef struct {int unused;} FIL;
typedef struct {int unused;} FATFS;
typedef unsigned UINT;
typedef uint64_t FSIZE_t;
typedef int FRESULT;
#define FR_OK 0
#define FR_DENIED 7
#define FR_WRITE_PROTECTED 10
#define FR_DISK_ERR 1
#define FA_READ 1
#define FA_WRITE 2
FRESULT f_mount(FATFS*, const char*, unsigned);
FRESULT f_lseek(FIL*, FSIZE_t);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_sync(FIL*);
FRESULT f_close(FIL*);
FRESULT f_open(FIL*, const char*, unsigned);
FSIZE_t f_size(FIL*);
#endif
