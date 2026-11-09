#ifndef PC_TRANSPORTER_MEMORY_H
#define PC_TRANSPORTER_MEMORY_H

#define PCT_DDR_BASE        0x3F900000U
#define PCT_RAM_BYTES       0xC0000U
#define PCT_CGA_RAM_BASE    0xB8000U
#define PCT_CGA_RAM_BYTES   0x04000U

/* CPU0 owns EMS backing. The FPGA only sees the ordinary guest RAM above. */
#define PCT_EMS_DDR_BASE    0x3FA00000U
#define PCT_EMS_FRAME_BASE  0xD0000U
#define PCT_EMS_WINDOWS     4U
#define PCT_EMS_PAGE_BYTES  0x4000U
#define PCT_EMS_PAGES       256U
#define PCT_EMS_BYTES       (PCT_EMS_PAGES * PCT_EMS_PAGE_BYTES)

#endif
