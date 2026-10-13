#include "qspi_nor.h"

#include <string.h>

#include "xparameters.h"
#include "xstatus.h"

#define CMD_WREN         0x06U
#define CMD_RDSR1        0x05U
#define CMD_RDID         0x9FU
#define CMD_READ_3B      0x03U
#define CMD_READ_4B      0x13U
#define CMD_QUAD_READ    0x6BU
#define CMD_RDSR2        0x35U
#define CMD_PP_3B        0x02U
#define CMD_PP_4B        0x12U
#define CMD_SE64K_3B     0xD8U
#define CMD_SE64K_4B     0xDCU
#define CMD_EN4B         0xB7U

#define SR_WIP           0x01U

#define QSPI_XFER_MAX    (5U + 256U)
#define QSPI_READ_CHUNK  4096U
#define QSPI_READ_BUFFER (QSPI_READ_CHUNK + 8U)
#define QSPI_QUAD_IO_CFG 0x0000016BU /* 6Bh, eight dummy clocks, no linear AXI. */
#define QSPI_3B_LIMIT    0x01000000U
#define QSPI_PROBE_BYTES 64U

/* XQspiPs_PolledTransfer loads command words as u32, even for short commands.
 * Pad and align the read buffers. CPU0 owns this non-reentrant NOR service;
 * these buffers never alias the CPU1 render mailbox or a linear flash map. */
static uint8_t read_tx[QSPI_READ_BUFFER] __attribute__((aligned(4)));
static uint8_t read_rx[QSPI_READ_BUFFER] __attribute__((aligned(4)));

static int qspi_slow_io(qspi_nor_t *n)
{
    if (!n->controller_ready ||
        XQspiPs_SetOptions(&n->qspi, XQSPIPS_FORCE_SSELECT_OPTION) != XST_SUCCESS ||
        XQspiPs_SetClkPrescaler(&n->qspi, XQSPIPS_CLK_PRESCALE_32) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    XQspiPs_SetLqspiConfigReg(&n->qspi, 0U);
    return XST_SUCCESS;
}

static int qspi_xfer_selected(qspi_nor_t *n, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    int status = XQspiPs_SetSlaveSelect(&n->qspi);
    if (status != XST_SUCCESS) return status;
    return XQspiPs_PolledTransfer(&n->qspi, (u8 *)tx, (u8 *)rx, len);
}

static int qspi_xfer(qspi_nor_t *n, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    /* Status, WREN, page program, erase and address-mode commands always use
     * the original single-bit clock. Fast reads never leave a write context
     * in quad/linear mode. The driver deasserts CS after every transfer. */
    if (qspi_slow_io(n) != XST_SUCCESS) return XST_FAILURE;
    return qspi_xfer_selected(n, tx, rx, len);
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
    uint8_t tx[4] __attribute__((aligned(4))) = { CMD_WREN, 0U, 0U, 0U };
    return qspi_xfer(n, tx, NULL, 1U);
}

static int qspi_read_status1(qspi_nor_t *n, uint8_t *sr)
{
    uint8_t tx[4] __attribute__((aligned(4))) = { CMD_RDSR1, 0U, 0U, 0U };
    uint8_t rx[4] __attribute__((aligned(4))) = { 0U, 0U, 0U, 0U };
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
    uint8_t tx[4] __attribute__((aligned(4))) = { CMD_EN4B, 0U, 0U, 0U };

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
    uint8_t tx[4] __attribute__((aligned(4))) = { CMD_RDID, 0U, 0U, 0U };
    uint8_t rx[4] __attribute__((aligned(4))) = { 0U, 0U, 0U, 0U };
    int rc = qspi_xfer(n, tx, rx, 4U);

    if (rc == XST_SUCCESS) {
        id[0] = rx[1];
        id[1] = rx[2];
        id[2] = rx[3];
    }
    return rc;
}

static int qspi_read_register(qspi_nor_t *n, uint8_t command, uint8_t *value)
{
    uint8_t tx[4] __attribute__((aligned(4))) = {0U, 0U, 0U, 0U};
    uint8_t rx[4] __attribute__((aligned(4))) = {0U, 0U, 0U, 0U};
    tx[0] = command;
    if (qspi_xfer(n, tx, rx, 2U) != XST_SUCCESS) return XST_FAILURE;
    *value = rx[1];
    return XST_SUCCESS;
}

static int qspi_read_mode(qspi_nor_t *n, uint32_t addr, uint8_t *out,
                          uint32_t len, int quad)
{
    uint32_t saved_options, saved_linear, off = 0U;
    uint8_t saved_prescaler;
    const uint32_t hdr = 1U + (uint32_t)n->addr_bytes + (quad ? 1U : 0U);
    const uint32_t options = XQSPIPS_FORCE_SSELECT_OPTION |
        (quad ? XQSPIPS_HOLD_B_DRIVE_OPTION : 0U);
    const uint8_t prescaler = quad ? XQSPIPS_CLK_PRESCALE_8 : XQSPIPS_CLK_PRESCALE_32;
    int rc = XST_FAILURE;

    if (!n->controller_ready) return XST_FAILURE;
    saved_options = XQspiPs_GetOptions(&n->qspi);
    saved_linear = XQspiPs_GetLqspiConfigReg(&n->qspi);
    saved_prescaler = XQspiPs_GetClkPrescaler(&n->qspi);
    if (XQspiPs_SetOptions(&n->qspi, options) != XST_SUCCESS ||
        XQspiPs_SetClkPrescaler(&n->qspi, prescaler) != XST_SUCCESS) goto restore;
    /* The retained FSBL uses this exact 6Bh/eight-dummy-clock IO setup and
     * divider8. No linear enable bit, CPU mapping or flash QE write is used. */
    XQspiPs_SetLqspiConfigReg(&n->qspi, quad ? QSPI_QUAD_IO_CFG : 0U);
    if (XQspiPs_GetClkPrescaler(&n->qspi) != prescaler ||
        XQspiPs_GetLqspiConfigReg(&n->qspi) != (quad ? QSPI_QUAD_IO_CFG : 0U)) goto restore;
    while (off < len) {
        uint32_t chunk = len - off;
        int status;
        if (chunk > QSPI_READ_CHUNK) chunk = QSPI_READ_CHUNK;
        memset(read_tx, 0, hdr + chunk);
        read_tx[0] = quad ? CMD_QUAD_READ :
            (n->addr_bytes == 4U ? CMD_READ_4B : CMD_READ_3B);
        qspi_pack_addr(read_tx + 1U, n->addr_bytes, addr + off);
        status = XQspiPs_SetSlaveSelect(&n->qspi);
        if (status != XST_SUCCESS) goto restore;
        ++n->read_transactions;
        if (XQspiPs_PolledTransfer(&n->qspi, read_tx, read_rx, hdr + chunk) != XST_SUCCESS)
            goto restore;
        memcpy(out + off, read_rx + hdr, chunk);
        off += chunk;
    }
    rc = XST_SUCCESS;
restore:
    /* All transfers have ended before restoring configuration. Failure to
     * restore a busy/broken controller blocks subsequent flash mutations. */
    if (XQspiPs_SetOptions(&n->qspi, saved_options) != XST_SUCCESS ||
        XQspiPs_SetClkPrescaler(&n->qspi, saved_prescaler) != XST_SUCCESS) {
        n->controller_ready = 0U;
        return XST_FAILURE;
    }
    XQspiPs_SetLqspiConfigReg(&n->qspi, saved_linear);
    if (XQspiPs_GetOptions(&n->qspi) != saved_options ||
        XQspiPs_GetClkPrescaler(&n->qspi) != saved_prescaler ||
        XQspiPs_GetLqspiConfigReg(&n->qspi) != saved_linear) {
        n->controller_ready = 0U;
        return XST_FAILURE;
    }
    return rc;
}

static int qspi_known_quad_part(const qspi_nor_t *n, uint8_t *qe_command, uint8_t *qe_mask)
{
    /* Exact 3 V, 128 Mbit identities for the manufacturers supported by the
     * retained FSBL. Merely matching density never enables a faster mode. */
    static const uint8_t parts[][4] = {
        {0x20U, 0xBAU, CMD_RDSR1, 0U},     /* Micron N25Q/MT25QL128 */
        {0x01U, 0x20U, CMD_RDSR2, 0x02U}, /* Spansion S25FL128 */
        {0xEFU, 0x40U, CMD_RDSR2, 0x02U}, /* Winbond W25Q128 */
        {0xEFU, 0x70U, CMD_RDSR2, 0x02U}, /* Winbond W25Q128JV */
        {0xC2U, 0x20U, CMD_RDSR1, 0x40U}, /* Macronix MX25L128 */
        {0x9DU, 0x60U, CMD_RDSR1, 0x40U}  /* ISSI IS25LP128 */
    };
    uint32_t i;
    if (n->jedec_id[2] != 0x18U) return 0;
    for (i = 0U; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (n->jedec_id[0] == parts[i][0] && n->jedec_id[1] == parts[i][1]) {
            *qe_command = parts[i][2];
            *qe_mask = parts[i][3];
            return 1;
        }
    }
    return 0;
}

static int qspi_board_quad_capable(void)
{
#if defined(XPAR_XQSPIPS_0_QSPI_BUS_WIDTH) && defined(XPAR_XQSPIPS_0_CONNECTION_MODE)
    return XPAR_XQSPIPS_0_QSPI_BUS_WIDTH == 2U && XPAR_XQSPIPS_0_CONNECTION_MODE == 0U;
#elif defined(XPAR_QSPI_QSPI_BUS_WIDTH) && defined(XPAR_QSPI_CONNECTION_MODE)
    return XPAR_QSPI_QSPI_BUS_WIDTH == 2U && XPAR_QSPI_CONNECTION_MODE == 0U;
#else
    return 0;
#endif
}

static void qspi_qualify_quad(qspi_nor_t *n)
{
    static const uint32_t addresses[] = {
        0x00000020U, 0x00200021U, 0x00800023U, 0x00FE0000U, 0x00FFFFC0U
    };
    uint8_t reference[QSPI_PROBE_BYTES], candidate[QSPI_PROBE_BYTES];
    uint8_t qe_command, qe_mask, sr;
    uint32_t i;
    if (n->addr_bytes != 3U || n->capacity_bytes != QSPI_3B_LIMIT ||
        n->input_clock_hz == 0U || n->input_clock_hz > 200000000U ||
        !qspi_board_quad_capable() || !qspi_known_quad_part(n, &qe_command, &qe_mask)) return;
    n->quad_probe_failed = 1U;
    if (qspi_read_status1(n, &sr) != XST_SUCCESS || (sr & SR_WIP) != 0U) return;
    if (qe_mask != 0U &&
        (qspi_read_register(n, qe_command, &sr) != XST_SUCCESS || (sr & qe_mask) == 0U)) return;
    for (i = 0U; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        if (qspi_read_mode(n, addresses[i], reference, QSPI_PROBE_BYTES, 0) != XST_SUCCESS) return;
        /* A real Zynq boot header is a non-erased, non-uniform reference.
         * All-FF/00 reads from a disconnected data bus cannot qualify quad. */
        if (i == 0U && (memcmp(reference, "\x66\x55\x99\xAA\x58\x4E\x4C\x58", 8U) != 0)) return;
        if (qspi_read_mode(n, addresses[i], candidate, QSPI_PROBE_BYTES, 1) != XST_SUCCESS ||
            memcmp(reference, candidate, QSPI_PROBE_BYTES) != 0) return;
    }
    n->quad_read_enabled = 1U;
    n->quad_probe_failed = 0U;
}

int qspi_nor_init(qspi_nor_t *n, uint8_t addr_bytes, uint32_t sector_size)
{
    XQspiPs_Config cfg = {0};
    int status;
    uint8_t jedec_id[3] = { 0U, 0U, 0U };

    if (!n || (addr_bytes != 3U && addr_bytes != 4U) || sector_size == 0U ||
        (sector_size & (sector_size - 1U)) != 0U) {
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

    status = XQspiPs_SetClkPrescaler(&n->qspi, XQSPIPS_CLK_PRESCALE_32);
    if (status != XST_SUCCESS) return status;
    /* Preferred mode for SPI NOR command sequences: manual CS (force select) with driver-managed per-transfer deassert. */
    status = XQspiPs_SetOptions(&n->qspi, XQSPIPS_FORCE_SSELECT_OPTION);
    if (status != XST_SUCCESS) {
        return status;
    }
    n->controller_ready = 1U;
    n->input_clock_hz = cfg.InputClockHz;

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

    status = qspi_enter_4byte_mode_if_needed(n);
    if (status != XST_SUCCESS) return status;
    qspi_qualify_quad(n);
    return n->controller_ready ? XST_SUCCESS : XST_FAILURE;
}

uint32_t qspi_nor_capacity_bytes(const qspi_nor_t *n)
{
    if (!n) {
        return 0U;
    }
    return n->capacity_bytes;
}

void qspi_nor_get_read_stats(const qspi_nor_t *n, qspi_nor_read_stats_t *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    stats->chunk_bytes = QSPI_READ_CHUNK;
    if (n == NULL) return;
    stats->bytes = n->read_bytes;
    stats->transactions = n->read_transactions;
    stats->fallback_count = n->read_fallback_count;
    stats->quad_enabled = n->quad_read_enabled;
    stats->probe_failed = n->quad_probe_failed;
    stats->clock_hz = n->input_clock_hz / (n->quad_read_enabled ? 8U : 32U);
    memcpy(stats->jedec_id, n->jedec_id, sizeof(stats->jedec_id));
}

const char *qspi_nor_read_mode(const qspi_nor_t *n)
{
    return n != NULL && n->quad_read_enabled != 0U ? "quad-io" : "spi";
}

static int qspi_range_valid(const qspi_nor_t *n, uint32_t addr, uint32_t len)
{
    const uint32_t capacity = n->capacity_bytes != 0U ? n->capacity_bytes :
        (n->addr_bytes == 3U ? QSPI_3B_LIMIT : UINT32_MAX);
    if ((n->addr_bytes != 3U && n->addr_bytes != 4U) || addr > capacity) return 0;
    return len <= capacity - addr;
}

int qspi_nor_read(qspi_nor_t *n, uint32_t addr, void *dst, uint32_t len)
{
    int rc;
    if (!n || !dst || !n->controller_ready) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }

    if (len == 0U) return XST_SUCCESS;
    rc = qspi_read_mode(n, addr, (uint8_t *)dst, len, n->quad_read_enabled != 0U);
    if (rc != XST_SUCCESS && n->quad_read_enabled != 0U) {
        n->quad_read_enabled = 0U;
        n->quad_probe_failed = 1U;
        if (!n->controller_ready) return XST_FAILURE;
        ++n->read_fallback_count;
        /* Replace the entire public result, including any already copied
         * quad chunks, from the same starting address in conservative SPI. */
        rc = qspi_read_mode(n, addr, (uint8_t *)dst, len, 0);
    }
    if (rc == XST_SUCCESS) n->read_bytes += len;
    return rc;
}

int qspi_nor_erase_region(qspi_nor_t *n, uint32_t addr, uint32_t len)
{
    uint8_t tx[8] __attribute__((aligned(4)));
    uint32_t start;
    uint32_t end;
    uint32_t a;
    uint32_t hdr;

    if (!n || !n->controller_ready || n->sector_size == 0U ||
        (n->sector_size & (n->sector_size - 1U)) != 0U) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }
    if (len == 0U) return XST_SUCCESS;
    start = addr & ~(n->sector_size - 1U);
    {
        const uint64_t rounded = ((uint64_t)addr + len + n->sector_size - 1U) &
            ~((uint64_t)n->sector_size - 1U);
        if (rounded > UINT32_MAX ||
            (n->capacity_bytes != 0U && rounded > n->capacity_bytes)) return XST_FAILURE;
        end = (uint32_t)rounded;
    }
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
    uint8_t tx[QSPI_XFER_MAX + 3U] __attribute__((aligned(4)));
    const uint8_t *in = (const uint8_t *)src;
    uint32_t off = 0U;
    uint32_t hdr;

    if (!n || !src || !n->controller_ready || n->page_size == 0U ||
        n->page_size > 256U || (n->page_size & (n->page_size - 1U)) != 0U) {
        return XST_FAILURE;
    }
    if (!qspi_range_valid(n, addr, len)) {
        return XST_FAILURE;
    }
    hdr = 1U + (uint32_t)n->addr_bytes;

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

        /* The vendor driver clocks a full final TX word for opcode02h.
         * FF padding leaves any rounded/wrapped extra NOR bytes unchanged. */
        memset(tx, 0xFF, sizeof(tx));
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
