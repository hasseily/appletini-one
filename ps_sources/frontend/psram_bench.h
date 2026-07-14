#ifndef PSRAM_BENCH_H
#define PSRAM_BENCH_H

#include <stdint.h>

#include "xiltimer.h"

#define PSRAM_WIN_BASE       0x46000000U
#define PSRAM_WIN_SIZE       0x01000000U
#define PSRAM_COPY_BYTES     0x10000U    /* 64 KiB benchmark chunk */

typedef struct {
    uint8_t mapped;
    uint8_t use_memcpy;
    uint8_t stress_mode;
    uint8_t running;
    uint8_t clk_div;
    uint8_t clk_div_tuned;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t loop;
    uint32_t ok_count;
    uint32_t fail_count;
    uint32_t mismatch_count;
    uint32_t stress_off;
    uint32_t stress_passes;
    uint32_t last_status;
    XTime    last_copy_ticks;
    XTime    last_write_ticks;
    XTime    last_read_ticks;
    XTime    last_cmp_ticks;
    uint32_t last_rw_mibps_x100;
    uint32_t last_loop_mibps_x100;
    uint32_t last_rw_sec_per_mib_x100;
    uint32_t last_loop_sec_per_mib_x100;
    uint32_t avg_rw_mibps_x100;
    uint32_t avg_rw_sec_per_mib_x100;
    uint32_t avg_loop_sec_per_mib_x100;
    uint64_t bench_total_ticks;
    uint64_t bench_total_rw_ticks;
    uint64_t bench_total_rw_bytes;
    uint64_t bench_total_loop_bytes;
    char msg[96];
} psram_ui_state_t;

void psram_bench_runtime_map(void);
void psram_bench_init_defaults(psram_ui_state_t *p);
void psram_bench_reset_counters(psram_ui_state_t *p);
int psram_bench_startup(psram_ui_state_t *p);
int psram_bench_step(psram_ui_state_t *p);

#endif

int psram_calibrate_dcount(uint32_t uart_base);

int psram_bank_probe(uint32_t uart_base);
