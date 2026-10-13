#ifndef APPLETINI_IMAGE_VERSIONS_H
#define APPLETINI_IMAGE_VERSIONS_H

/*
 * Manually bump these when generating a new boot image or firmware image.
 * They are displayed by boot_updater (UART) and text_ui_test (UART + Home UI).
 */
#define APPLETINI_BOOT_IMAGE_VERSION_SHORT      "B2.0.5"
#define APPLETINI_BOOT_IMAGE_VERSION_FULL       "Boot B2.0.5"

#define APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT  "F1.3.0-dev8"
#define APPLETINI_FIRMWARE_IMAGE_VERSION_FULL   "Firmware F1.3.0-dev8"

/* Current updater/flash layout configuration shown in firmware UI. */
#define APPLETINI_FLASH_LAYOUT_LABEL            "QSPI 16MB Appletini + Multitini"
#define APPLETINI_FLASH_TYPE_LABEL              "qspi-x4-single"
#define APPLETINI_FLASH_GOLDEN_OFFSET           0x00000000U
#define APPLETINI_FLASH_GOLDEN_SIZE             0x00100000U
#define APPLETINI_FLASH_GOLDEN_TRIAL_OFFSET     0x00100000U
#define APPLETINI_FLASH_FIRMWARE_OFFSET         0x00200000U
#define APPLETINI_FLASH_FIRMWARE_SIZE           0x00600000U
#define APPLETINI_FLASH_METADATA_OFFSET         0x00FE0000U
#define APPLETINI_FLASH_METADATA_SIZE           0x00020000U
#define APPLETINI_UPDATE_FILE_NAME              "FIRMWARE.BIN"

#endif
