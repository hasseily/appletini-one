#include "updater_layout.h"

static const flash_layout_t g_layout_16mb_single_slot = {
    .qspi_addr_bytes = 3U,
    .erase_size_bytes = 0x00010000U,
    .golden   = { 0x00000000U, 0x00200000U }, /* 2 MiB */
    .firmware = { 0x00200000U, 0x00DF0000U }, /* up to final metadata sector */
    .metadata = { 0x00FF0000U, 0x00010000U }  /* final 64 KiB sector */
};

const flash_layout_t *updater_layout_active(void)
{
    return &g_layout_16mb_single_slot;
}

const char *updater_layout_name(const flash_layout_t *layout)
{
    if (!layout) {
        return "unknown";
    }
    return "qspi_16mb_single_slot";
}

uint32_t updater_layout_firmware_offset(const flash_layout_t *layout)
{
    if (!layout) {
        return 0U;
    }
    return layout->firmware.offset;
}
