/* Reuse the shared in-memory FatFs types, adding seek for image validation. */
#ifndef ONLINE_UPDATE_HOST_FF_H
#define ONLINE_UPDATE_HOST_FF_H
#include "../http_service_host/ff.h"
FRESULT f_lseek(FIL *file,size_t offset);
#endif
