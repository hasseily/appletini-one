/* Actual production mount/directory/open helpers; a mock live download FIL
 * becomes invalid whenever FatFs registers another object for volume zero. */
#include <assert.h>
#include <stdio.h>
typedef int FRESULT;
typedef struct { int unused; } FATFS;
typedef struct { int unused; } FIL;
enum { FR_OK, FR_DISK_ERR, FR_NOT_READY, FR_NO_FILE, FR_DENIED, FR_EXIST,
       FR_NOT_ENABLED, FR_WRITE_PROTECTED };
#define PRINTER_SERVICE_DIR "0:/printouts"
#define FA_CREATE_ALWAYS 8
#define FA_WRITE 2
static FATFS g_printer_fs;
static unsigned mounts, initializes, mkdirs, opens;
static int registered, live_download, invalidated;
static FRESULT mkdir_result, open_result, mount_result;
static FRESULT f_mount(FATFS *fs, const char *path, int immediate)
{
    (void)path; (void)immediate; ++mounts;
    if (live_download) invalidated = 1;
    registered = fs != NULL && mount_result == FR_OK;
    return mount_result;
}
static int disk_initialize(int drive) { (void)drive; ++initializes; return 0; }
static FRESULT f_mkdir(const char *path)
{ (void)path; ++mkdirs; return registered ? mkdir_result : FR_NOT_ENABLED; }
static FRESULT f_open(FIL *file, const char *path, int flags)
{ (void)file; (void)path; assert(flags == (FA_CREATE_ALWAYS | FA_WRITE)); ++opens;
  return registered ? open_result : FR_NOT_ENABLED; }
#include "printer_sd_helpers.inc"

static void reset(int mounted)
{
    mounts = initializes = mkdirs = opens = 0;
    registered = live_download = mounted; invalidated = 0;
    mkdir_result = FR_EXIST; open_result = mount_result = FR_OK;
}
int main(void)
{
    FIL file;
    const FRESULT errors[] = {FR_DISK_ERR, FR_NOT_READY, FR_DENIED, FR_WRITE_PROTECTED};
    reset(1); assert(printer_ensure_dir() == FR_OK);
    assert(!printer_open_output(&file, "0:/printouts/print_0001.png"));
    assert(!mounts && !initializes && !invalidated && mkdirs == 1 && opens == 1);
    reset(1); mkdir_result = FR_OK; assert(printer_ensure_dir() == FR_OK && !mounts);
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        reset(1); mkdir_result = errors[i]; assert(printer_ensure_dir() == errors[i]);
        assert(!mounts && !initializes && !invalidated);
        reset(1); open_result = errors[i];
        assert(printer_open_output(&file, "0:/printouts/print_0001.png") == errors[i]);
        assert(!mounts && !initializes && !invalidated && opens == 1);
    }
    reset(0); assert(printer_ensure_dir() == FR_OK);
    assert(mounts == 1 && mkdirs == 2 && !initializes);
    reset(0); assert(printer_open_output(&file, "0:/printouts/print_0001.png") == FR_OK);
    assert(mounts == 1 && opens == 2 && !initializes);
    reset(0); mount_result = FR_DISK_ERR;
    assert(printer_ensure_dir() == FR_DISK_ERR && mounts == 3 && initializes == 1);
    puts("PASS printer SD ownership: mounted volume reused, live download preserved, mkdir/open errors do not remount, absent volume initializes");
    return 0;
}
