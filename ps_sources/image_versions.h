#ifndef APPLETINI_IMAGE_VERSIONS_H
#define APPLETINI_IMAGE_VERSIONS_H

/*
 * Manually bump these when generating a new boot image or firmware image.
 * They are displayed by boot_updater (UART) and text_ui_test (UART + Home UI).
 */
#define APPLETINI_BOOT_IMAGE_VERSION_SHORT      "B1.1.0"
#define APPLETINI_BOOT_IMAGE_VERSION_FULL       "Boot B1.1.0"

#define APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT  "F0.9.0"
#define APPLETINI_FIRMWARE_IMAGE_VERSION_FULL   "Firmware F0.9.0"

/* Current updater/flash layout configuration shown in firmware UI. */
#define APPLETINI_FLASH_LAYOUT_LABEL            "QSPI 16MB single-slot"
#define APPLETINI_FLASH_TYPE_LABEL              "qspi-x4-single"
#define APPLETINI_FLASH_GOLDEN_OFFSET           0x00000000U
#define APPLETINI_FLASH_GOLDEN_SIZE             0x00200000U
#define APPLETINI_FLASH_FIRMWARE_OFFSET         0x00200000U
#define APPLETINI_FLASH_FIRMWARE_SIZE           0x00DF0000U
#define APPLETINI_FLASH_METADATA_OFFSET         0x00FF0000U
#define APPLETINI_FLASH_METADATA_SIZE           0x00010000U
#define APPLETINI_UPDATE_FILE_NAME              "FIRMWARE.BIN"

#endif
