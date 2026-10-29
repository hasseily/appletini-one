/* No SD access in the network-state fixture. These fail if setup is reached. */
#ifndef HTTP_HOST_FF_H
#define HTTP_HOST_FF_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned UINT;
typedef int FRESULT;
typedef struct { size_t size, position; int slot, mode; } FIL;
typedef struct { size_t fsize; uint8_t fattrib; } FILINFO;
typedef struct { int unused; } FATFS;
#define FR_OK 0
#define FR_DISK_ERR 1
#define FR_NO_FILE 4
#define FR_DENIED 7
#define FR_EXIST 8
#define FR_INVALID_OBJECT 9
#define FR_WRITE_PROTECTED 10
#define FR_NOT_ENABLED 12
#define AM_DIR 0x10
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_NEW 4
#define FA_CREATE_ALWAYS 8
#define f_size(file) ((file)->size)
#ifdef HTTP_HOST_FAKE_SD
FRESULT f_stat(const char *p,FILINFO *i);
FRESULT f_mount(FATFS *f,const char *p,int n);
FRESULT f_open(FIL *f,const char *p,int m);
FRESULT f_close(FIL *f);
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got);
FRESULT f_write(FIL *f,const void *p,UINT n,UINT *got);
FRESULT f_sync(FIL *f);
FRESULT f_unlink(const char *p);
FRESULT f_mkdir(const char *p);
FRESULT f_rename(const char *old,const char *new_name);
#else
static inline int f_stat(const char *p,FILINFO *i) { (void)p;(void)i;return FR_NO_FILE; }
static inline int f_mount(FATFS *f,const char *p,int n) { (void)f;(void)p;(void)n;return -1; }
static inline int f_open(FIL *f,const char *p,int m) { (void)f;(void)p;(void)m;return -1; }
static inline int f_close(FIL *f) { (void)f;return -1; }
static inline int f_read(FIL *f,void *p,UINT n,UINT *got) { (void)f;(void)p;(void)n;*got=0;return -1; }
static inline int f_write(FIL *f,const void *p,UINT n,UINT *got) { (void)f;(void)p;(void)n;*got=0;return -1; }
static inline int f_sync(FIL *f) { (void)f;return -1; }
static inline int f_unlink(const char *p) { (void)p;return -1; }
static inline int f_mkdir(const char *p) { (void)p;return -1; }
static inline int f_rename(const char *p,const char *n) { (void)p;(void)n;return -1; }
#endif
#endif
