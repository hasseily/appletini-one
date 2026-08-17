#ifndef FTP_SD_SERVICE_H
#define FTP_SD_SERVICE_H

#include <stddef.h>
#include <stdint.h>

/* Anonymous, passive-only FTP server for exclusive SD-card maintenance.
 * The caller must suspend every other SD owner and hide Apple-side Uthernet
 * before start, then keep calling poll from the main loop. */
int ftp_sd_service_start(char *detail, size_t detail_len);
void ftp_sd_service_stop(void);
void ftp_sd_service_poll(void);
uint8_t ftp_sd_service_active(void);

#endif
