#include "qspi_nor.h"

#include <string.h>

#include "xparameters.h"
#include "xstatus.h"

#define CMD_WREN         0x06U
#define CMD_RDSR1        0x05U
#define CMD_RDID         0x9FU
#define CMD_READ_3B      0x03U
#define CMD_READ_4B      0x13U
#define CMD_PP_3B        0x02U
#define CMD_PP_4B        0x12U
#define CMD_SE64K_3B     0xD8U
#define CMD_SE64K_4B     0xDCU
#define CMD_EN4B         0xB7U

#define SR_WIP           0x01U

#define QSPI_XFER_MAX    (5U + 256U)

static int qspi_xfer(qspi_nor_t *n, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    int status;

    /* In manual-CS mode, select the flash before each command transfer. The driver deasserts CS at transfer end. */
    status = XQspiPs_SetSlaveSelect(&n->qspi);
    if (status != XST_SUCCESS) {
        return status;
    }

    return XQspiPs_PolledTransfer(&n->qspi, (u8 *)tx, (u8 *)rx, len);
}

static void qspi_pack_addr(uint8_t *p, uint8_t addr_bytes, uint32_t addr)
{
    if (addr_bytes == 4U) {
        p[0] = (uint8_t)(addr >> 24);
        p[1] = (uint8_t)(addr >> 16);
        p[2] = (uint8_t)(addr >> 8);
        p[3] = (uint8_t)(addr >> 0);
    } else {
        p[0] = (uint8_t)(addr >> 16);
        p[1] = (uint8_t)(addr >> 8);
        p[2] = (uint8_t)(addr >> 0);
    }
}

static int qspi_write_enable(qspi_nor_t *n)
{
    uint8_t tx[1] = { CMD_WREN };
    return qspi_xfer(n, tx, NULL, 1U);
}

static int qspi_read_status1(qspi_nor_t *n, uint8_t *sr)
{
    uint8_t tx[2] = { CMD_RDSR1, 0 };
    uint8_t rx[2] = { 0, 0 };
    int rc = qspi_xfer(n, tx, rx, 2U);
    if (rc == XST_SUCCESS) {
        *sr = rx[1];
    }
    return rc;
}

static int qspi_wait_ready(qspi_nor_t *n, uint32_t polls)
{
    uint8_t sr = 0;
    while (polls--) {
        if (qspi_read_status1(n, &sr) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        if ((sr & SR_WIP) == 0U) {
            return XST_SUCCESS;
        }
    }
    return XST_FAILURE;
}

static int qspi_enter_4byte_mode_if_needed(qspi_nor_t *n)
{
    uint8_t tx[1] = { CMD_EN4B };

    if (n->addr_bytes != 4U) {
        return XST_SUCCESS;
    }

    /* Parts that require 4-byte addressing generally support B7h. */
    if (qspi_write_enable(n) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    return qspi_xfer(n, tx, NULL, 1U);
}

static uint32_t qspi_capacity_from_density(uint8_t density)
{
    if (density >= 16U && density <= 31U) {
        return (uint32_t)(1UL << density);
    }
    return 0U;
}

static int qspi_read_jedec_id(qspi_nor_t *n, uint8_t id[3])
{
    uint8_t tx[4] = { CMD_RDID, 0, 0, 0 };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    int rc = qspi_xfer(n, tx, rx, 4U);

    if (rc == XST_SUCCESS) {
        id[0] = rx[1];
        id[1] = rx[2];
        id[2] = rx[3];
    }
    return rc;
}

int qspi_nor_init(qspi_nor_t *n, uint8_t addr_bytes, uint32_t sector_size)
{
    XQspiPs_Config cfg;
    int status;
    uint8_t jedec_id[3] = { 0U, 0U, 0U };

    if (!n) {
        return XST_FAILURE;
    }
    memset(n, 0, sizeof(*n));

    cfg.BaseAddress = XPAR_XQSPIPS_0_BASEADDR;
#ifdef XPAR_XQSPIPS_0_CLOCK_FREQ
    cfg.InputClockHz = XPAR_XQSPIPS_0_CLOCK_FREQ;
#else
    cfg.InputClockHz = 50000000U;
#endif

    status = XQspiPs_CfgInitialize(&n->qspi, &cfg, cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        return status;
    }

    XQspiPs_SetClkPrescaler(&n->qspi, XQSPIPS_CLK_PRESCALE_32);
    /* Preferred mode for SPI NOR command sequences: manual CS (force select) with driver-managed per-transfer deassert. */
    status = XQspiPs_SetOptions(&n->qspi, XQSPIPS_FORCE_SSELECT_OPTION);
    if (status != XST_SUCCESS) {
        return status;
    }

    n->addr_bytes = addr_bytes;
    n->page_size = 256U;
    n->sector_size = sector_size;
    n->capacity_bytes = 0U;
    n->jedec_id[0] = 0U;
    n->jedec_id[1] = 0U;
    n->jedec_id[2] = 0U;

    if (qspi_read_jedec_id(n, jedec_id) == XST_SUCCESS) {
        n->jedec_id[0] = jedec_id[0];
        n->jedec_id[1] = jedec_id[1];
        n->jedec_id[2] = jedec_id[2];
        n->capacity_bytes = qspi_capacity_from_density(jedec_id[2]);
        if (n->capacity_bytes != 0U && n->capacity_bytes <= 0x01000000U) {
            n->addr_bytes = 3U;
        }
    }
    n->supports_4byte_mode = (n->addr_bytes == 4U) ? 1U : 0U;

    return qspi_enter_4byte_mode_if_needed(n);
}

uint32_t qspi_nor_capacity_bytes(const qspi_nor_t *n)
{
    if (!n) {
        return 0U;
    }
    return n->capacity_bytes;
}

static int qspi_range_valid(const qspi_nor_t *n, uint32_t addr, uint32_t len)
{
    if (n->capacity_bytes == 0U) {
        return 1;
    }
    if (addr > n->capacity_bytes) {
        return 0;
    }
    return len <= (n->capacity_bytes - addr);
}

int qspi_nor_read(qspi_nor_t *n, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t tx[QSPI_XFER_MAX];
    uint8_t rx[QSPI_XFER_MAX];
    uint8_t *out = (uint8_t *)dst;
    uint32_t chunk;
    uint32_t hdr;
    uint32_t off = 0U;

    if (!n || !dst) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }

    hdr = 1U + (uint32_t)n->addr_bytes;

    while (off < len) {
        chunk = len - off;
        if (chunk > (QSPI_XFER_MAX - hdr)) {
            chunk = (QSPI_XFER_MAX - hdr);
        }

        memset(tx, 0, hdr + chunk);
        memset(rx, 0, hdr + chunk);
        tx[0] = (n->addr_bytes == 4U) ? CMD_READ_4B : CMD_READ_3B;
        qspi_pack_addr(&tx[1], n->addr_bytes, addr + off);

        if (qspi_xfer(n, tx, rx, hdr + chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        memcpy(out + off, &rx[hdr], chunk);
        off += chunk;
    }
    return XST_SUCCESS;
}

int qspi_nor_erase_region(qspi_nor_t *n, uint32_t addr, uint32_t len)
{
    uint8_t tx[5];
    uint32_t start;
    uint32_t end;
    uint32_t a;
    uint32_t hdr;

    if (!n || n->sector_size == 0U) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }
    start = addr & ~(n->sector_size - 1U);
    end = (addr + len + n->sector_size - 1U) & ~(n->sector_size - 1U);
    hdr = 1U + (uint32_t)n->addr_bytes;

    for (a = start; a < end; a += n->sector_size) {
        memset(tx, 0, sizeof(tx));

        if (qspi_write_enable(n) != XST_SUCCESS) {
            return XST_FAILURE;
        }

        tx[0] = (n->addr_bytes == 4U) ? CMD_SE64K_4B : CMD_SE64K_3B;
        qspi_pack_addr(&tx[1], n->addr_bytes, a);
        if (qspi_xfer(n, tx, NULL, hdr) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        if (qspi_wait_ready(n, 2000000U) != XST_SUCCESS) {
            return XST_FAILURE;
        }
    }

    return XST_SUCCESS;
}

int qspi_nor_program(qspi_nor_t *n, uint32_t addr, const void *src, uint32_t len)
{
    uint8_t tx[QSPI_XFER_MAX];
    const uint8_t *in = (const uint8_t *)src;
    uint32_t off = 0U;
    uint32_t hdr = 1U + (uint32_t)n->addr_bytes;

    if (!n || !src) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }

    while (off < len) {
        uint32_t page_off = (addr + off) & (n->page_size - 1U);
        uint32_t page_rem = n->page_size - page_off;
        uint32_t chunk = len - off;
        if (chunk > page_rem) {
            chunk = page_rem;
        }
        if (chunk > (QSPI_XFER_MAX - hdr)) {
            chunk = (QSPI_XFER_MAX - hdr);
        }

        if (qspi_write_enable(n) != XST_SUCCESS) {
            return XST_FAILURE;
        }

        memset(tx, 0, hdr + chunk);
        tx[0] = (n->addr_bytes == 4U) ? CMD_PP_4B : CMD_PP_3B;
        qspi_pack_addr(&tx[1], n->addr_bytes, addr + off);
        memcpy(&tx[hdr], in + off, chunk);

        if (qspi_xfer(n, tx, NULL, hdr + chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        if (qspi_wait_ready(n, 200000U) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        off += chunk;
    }
    return XST_SUCCESS;
}
