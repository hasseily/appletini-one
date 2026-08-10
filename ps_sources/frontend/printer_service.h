#ifndef PRINTER_SERVICE_H
#define PRINTER_SERVICE_H

/* Virtual SSC printer backend. Drains the PL printer FIFO, runs the
 * ImageWriter II interpreter, and writes each rendered page as a PNG into
 * 0:/printouts on the SD card. A print job closes on an idle timeout or a
 * final form feed; a partial last page is flushed at job close. */

#include <stdint.h>

#define PRINTER_SERVICE_DIR "0:/printouts"
#define PRINTER_SERVICE_PATH_LEN 96

void printer_service_init(void);

/* Long page encodes call this between chunks so USB0 stays serviced
 * (same contract as the AD8088/Applicard service checkpoints). */
void printer_service_set_checkpoint(void (*checkpoint)(void));

void printer_service_poll(void);

/* Menu status helpers. */
uint32_t printer_service_pages_saved(void);
const char *printer_service_last_file(void);
uint8_t printer_service_job_active(void);

#endif /* PRINTER_SERVICE_H */
