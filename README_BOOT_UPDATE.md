# Boot and Firmware Update

Appletini One uses a permanent golden updater and one firmware slot in the
production 128 Mbit / 16 MB Quad SPI flash.

## Flash Layout

| Region | Offset | Size | Contents |
| --- | ---: | ---: | --- |
| Golden | `0x00000000` | `0x00200000` | `BOOT.BIN`: FSBL + `bootloader.elf` |
| Firmware | `0x00200000` | `0x00DF0000` | `FIRMWARE.BIN`: FSBL + bitstream + frontend |
| Metadata | `0x00FF0000` | `0x00010000` | Last verified update record |

The flash is configured as `qspi-x4-single`. Layout constants are defined in
`ps_sources/bootloader/updater_layout.c` and mirrored for the frontend in
`ps_sources/image_versions.h`.

## Boot Flow

1. Zynq BootROM loads the golden image at flash offset `0x00000000`.
2. Golden initializes UART, SD, and QSPI.
3. If the SD root contains `FIRMWARE.BIN`, golden validates and installs it.
4. Otherwise, golden boots the verified firmware slot.
5. If no verified firmware is available, golden remains in the serial monitor.

Golden selects the firmware slot through the Zynq multiboot register and a
software reset. The BootROM then loads the image at `0x00200000`.

## Update Guarantees

- The SD update path never erases or programs the golden region.
- The image must fit entirely inside the firmware slot.
- Every programmed byte is read back and checked before metadata is committed.
- A successful update renames `FIRMWARE.BIN` to `FIRMWARE.OK`.
- A failed update keeps `FIRMWARE.BIN` in place and does not mark the slot valid.
- Metadata is advisory; installation verifies the complete image.

Power loss can invalidate the single firmware slot. Recovery uses a valid
`FIRMWARE.BIN` on SD, the golden serial monitor, or direct QSPI programming.

## Serial Recovery

During golden's serial window, the host can interrupt automatic boot and send
an image with XMODEM-CRC. The helper handles the monitor protocol and can reboot
the running frontend into golden:

```bat
python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
```

The uploaded image is written to SD and installed through the same verified
update path as a manually copied file.

## Release Check

1. Regenerate the Vivado project and export a fresh XSA.
2. Regenerate and build the Vitis workspace.
3. Build `BOOT.BIN` and `FIRMWARE.BIN` from those outputs.
4. Install `FIRMWARE.BIN` through golden and confirm readback verification.
5. Confirm `FIRMWARE.OK`, metadata, frontend version, golden version, and RTL version.
6. Reboot without an update file and confirm a direct firmware-slot boot.
7. Confirm that an invalid image is rejected without changing verified metadata.
