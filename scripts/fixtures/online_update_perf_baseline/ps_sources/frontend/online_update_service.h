#ifndef ONLINE_UPDATE_SERVICE_H
#define ONLINE_UPDATE_SERVICE_H
#include "online_update_core.h"

/* Foreground only. poll performs work only while allowed and !busy. Losing
 * either condition cancels ownership and removes the service's partial file.
 * READY means a complete validated root file; no flash/reboot is performed. */
#if defined(APPLETINI_HTTP_SERVICE) || defined(ONLINE_UPDATE_HOST)
void online_update_init(const char *installed_f, const char *installed_b);
int online_update_check(void);
int online_update_start(uint8_t kind);
void online_update_cancel(void);
void online_update_poll(uint8_t allowed, uint8_t busy);
/* Terminal diagnostics persist until an accepted check/download. Cleanup
 * never replaces result, stage, or the original FatFs/validation failure. */
void online_update_status(online_update_snapshot_t *out);
int online_update_active(void);
const char *online_update_ready_path(void);
#else
static inline void online_update_init(const char *f,const char *b) { (void)f;(void)b; }
static inline int online_update_check(void) { return ONLINE_UPDATE_E_UNSAFE; }
static inline int online_update_start(uint8_t k) { (void)k;return ONLINE_UPDATE_E_UNSAFE; }
static inline void online_update_cancel(void) {}
static inline void online_update_poll(uint8_t a,uint8_t b) { (void)a;(void)b; }
static inline void online_update_status(online_update_snapshot_t *s)
{ if(s)*s=(online_update_snapshot_t){0}; }
static inline int online_update_active(void) { return 0; }
static inline const char *online_update_ready_path(void) { return ""; }
#endif
#endif
