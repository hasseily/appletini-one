#include "updater_layout.h"
#include "../lib/multiboot.h"

static const flash_layout_t g_layout_16mb_multiboot = {
    .qspi_addr_bytes = 3U,
    .erase_size_bytes = MB_SECTOR_SIZE,
    .golden = { MB_GOLDEN_OFFSET, MB_GOLDEN_SIZE * 2U },
    .firmware = { MB_APPLETINI_OFFSET, MB_APPLETINI_CAPACITY },
    .multitini = { MB_MULTITINI_OFFSET, MB_MULTITINI_CAPACITY },
    .metadata = { MB_METADATA_A_OFFSET, MB_SECTOR_SIZE * 2U }
};

const flash_layout_t *updater_layout_active(void)
{
    return &g_layout_16mb_multiboot;
}

const char *updater_layout_name(const flash_layout_t *layout)
{
    return layout ? "qspi_16mb_multiboot" : "unknown";
}

uint32_t updater_layout_firmware_offset(const flash_layout_t *layout)
{
    /* Display helper only. Actual boot selection validates both slots. */
    return layout ? layout->firmware.offset : 0U;
}
