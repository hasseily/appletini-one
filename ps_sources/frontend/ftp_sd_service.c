#include "ftp_sd_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "screenshot_service.h"
#include "uthernet2_control.h"

#define FTP_CONTROL_SOCKET       0U
#define FTP_DATA_SOCKET          1U
#define FTP_CONTROL_PORT         21U
#define FTP_PASSIVE_PORT         50000U
#define FTP_CONTROL_BUFFER_LEN   512U
#define FTP_DATA_BUFFER_LEN      1024U
#define FTP_PATH_LEN             256U
#define FTP_FAT_PATH_LEN         (FTP_PATH_LEN + 3U)

#define W5100_REG_RMSR           0x001AU
#define W5100_REG_TMSR           0x001BU
#define W5100_SOCKET_MEM_4_2_1_1 0x06U

#define W5100_SN_MR              0x00U
#define W5100_SN_CR              0x01U
#define W5100_SN_IR              0x02U
#define W5100_SN_SR              0x03U
#define W5100_SN_PORT            0x04U
#define W5100_SN_DIPR            0x0CU
#define W5100_SN_TX_FSR          0x20U
#define W5100_SN_TX_WR           0x24U
#define W5100_SN_RX_RSR          0x26U
#define W5100_SN_RX_RD           0x28U

#define W5100_SN_MR_TCP          0x01U
#define W5100_CR_OPEN            0x01U
#define W5100_CR_LISTEN          0x02U
#define W5100_CR_DISCON          0x08U
#define W5100_CR_CLOSE           0x10U
#define W5100_CR_SEND            0x20U
#define W5100_CR_RECV            0x40U
#define W5100_IR_CON             0x01U
#define W5100_IR_DISCON          0x02U
#define W5100_IR_RECV            0x04U
#define W5100_IR_TIMEOUT         0x08U
#define W5100_IR_SENDOK          0x10U
#define W5100_SR_CLOSED          0x00U
#define W5100_SR_INIT            0x13U
#define W5100_SR_LISTEN          0x14U
#define W5100_SR_ESTABLISHED     0x17U
#define W5100_SR_CLOSE_WAIT      0x1CU

#define W5100_TX_BASE_S0         0x4000U
#define W5100_TX_BASE_S1         0x5000U
#define W5100_RX_BASE_S0         0x6000U
#define W5100_RX_BASE_S1         0x7000U
#define W5100_MASK_S0            0x0FFFU
#define W5100_MASK_S1            0x07FFU

typedef enum {
    FTP_TRANSFER_NONE = 0,
    FTP_TRANSFER_LIST,
    FTP_TRANSFER_NLST,
    FTP_TRANSFER_RETR,
    FTP_TRANSFER_STOR
} ftp_transfer_t;

typedef struct {
    uint8_t active;
    uint8_t control_connected;
    uint8_t control_tx_pending;
    uint8_t close_control_after_tx;
    uint8_t logged_in;
    uint8_t data_listening;
    uint8_t data_connected;
    uint8_t data_tx_pending;
    uint8_t transfer_eof;
    uint8_t file_open;
    uint8_t dir_open;
    uint8_t write_changed;
    ftp_transfer_t transfer;
    FATFS fs;
    FIL file;
    DIR dir;
    uthernet2_network_config_t network;
    uint8_t control_peer[UTHERNET2_IPV4_LEN];
    char cwd[FTP_PATH_LEN];
    char rename_from[FTP_PATH_LEN];
    char control_rx[FTP_CONTROL_BUFFER_LEN];
    uint16_t control_rx_len;
    char control_out[FTP_CONTROL_BUFFER_LEN];
    uint16_t control_out_len;
    uint8_t data_buffer[FTP_DATA_BUFFER_LEN];
    uint16_t data_buffer_len;
} ftp_sd_state_t;

static ftp_sd_state_t g_ftp;

static uint16_t socket_reg(uint8_t socket, uint16_t offset)
{
    return (uint16_t)(0x0400U + ((uint16_t)socket * 0x0100U) + offset);
}

static uint16_t socket_tx_base(uint8_t socket)
{
    return socket == FTP_CONTROL_SOCKET ? W5100_TX_BASE_S0 : W5100_TX_BASE_S1;
}

static uint16_t socket_rx_base(uint8_t socket)
{
    return socket == FTP_CONTROL_SOCKET ? W5100_RX_BASE_S0 : W5100_RX_BASE_S1;
}

static uint16_t socket_mask(uint8_t socket)
{
    return socket == FTP_CONTROL_SOCKET ? W5100_MASK_S0 : W5100_MASK_S1;
}

static int w5100_read16(uint16_t addr, uint16_t *value)
{
    uint8_t bytes[2];

    if (value == NULL || uthernet2_read_block(addr, bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    *value = (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
    return 0;
}

static int w5100_read16_stable(uint16_t addr, uint16_t *value)
{
    uint16_t previous;
    uint16_t current;

    if (value == NULL || w5100_read16(addr, &previous) != 0) {
        return -1;
    }
    for (uint8_t attempt = 0U; attempt < 8U; ++attempt) {
        if (w5100_read16(addr, &current) != 0) {
            return -1;
        }
        if (current == previous) {
            *value = current;
            return 0;
        }
        previous = current;
    }
    return -1;
}

static int w5100_write16(uint16_t addr, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value >> 8U), (uint8_t)value
    };

    return uthernet2_write_block(addr, bytes, sizeof(bytes));
}

static int socket_command(uint8_t socket, uint8_t command)
{
    uint8_t value;

    if (uthernet2_write_reg(socket_reg(socket, W5100_SN_CR), command) != 0) {
        return -1;
    }
    for (uint16_t poll = 0U; poll < 1000U; ++poll) {
        if (uthernet2_read_reg(socket_reg(socket, W5100_SN_CR), &value) != 0) {
            return -1;
        }
        if (value == 0U) {
            return 0;
        }
    }
    return -1;
}

static int socket_status(uint8_t socket, uint8_t *status)
{
    return uthernet2_read_reg(socket_reg(socket, W5100_SN_SR), status);
}

static void socket_close(uint8_t socket)
{
    (void)socket_command(socket, W5100_CR_CLOSE);
    (void)uthernet2_write_reg(socket_reg(socket, W5100_SN_IR), 0xFFU);
}

/* Complete the TCP close handshake so clients see EOF on a finished data
 * transfer. W5100 CLOSE discards the socket immediately and does not send the
 * FIN that FTP clients wait for after the final payload byte. */
static void socket_disconnect(uint8_t socket)
{
    uint8_t status;

    if (socket_status(socket, &status) == 0 &&
        (status == W5100_SR_ESTABLISHED || status == W5100_SR_CLOSE_WAIT) &&
        socket_command(socket, W5100_CR_DISCON) == 0) {
        return;
    }
    socket_close(socket);
}

static int socket_listen(uint8_t socket, uint16_t port)
{
    uint8_t status;

    socket_close(socket);
    if (uthernet2_write_reg(socket_reg(socket, W5100_SN_MR), W5100_SN_MR_TCP) != 0 ||
        w5100_write16(socket_reg(socket, W5100_SN_PORT), port) != 0 ||
        socket_command(socket, W5100_CR_OPEN) != 0 ||
        socket_status(socket, &status) != 0 || status != W5100_SR_INIT ||
        socket_command(socket, W5100_CR_LISTEN) != 0 ||
        socket_status(socket, &status) != 0 || status != W5100_SR_LISTEN) {
        socket_close(socket);
        return -1;
    }
    return 0;
}

static int socket_ring_read(uint8_t socket,
                            uint16_t pointer,
                            uint8_t *dst,
                            uint16_t len)
{
    const uint16_t base = socket_rx_base(socket);
    const uint16_t mask = socket_mask(socket);

    while (len != 0U) {
        const uint16_t offset = (uint16_t)(pointer & mask);
        uint16_t chunk = (uint16_t)((mask + 1U) - offset);

        if (chunk > len) {
            chunk = len;
        }
        if (uthernet2_read_block((uint16_t)(base + offset), dst, chunk) != 0) {
            return -1;
        }
        pointer = (uint16_t)(pointer + chunk);
        dst += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 0;
}

static int socket_ring_write(uint8_t socket,
                             uint16_t pointer,
                             const uint8_t *src,
                             uint16_t len)
{
    const uint16_t base = socket_tx_base(socket);
    const uint16_t mask = socket_mask(socket);

    while (len != 0U) {
        const uint16_t offset = (uint16_t)(pointer & mask);
        uint16_t chunk = (uint16_t)((mask + 1U) - offset);

        if (chunk > len) {
            chunk = len;
        }
        if (uthernet2_write_block((uint16_t)(base + offset), src, chunk) != 0) {
            return -1;
        }
        pointer = (uint16_t)(pointer + chunk);
        src += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 0;
}

static int socket_receive(uint8_t socket,
                          uint8_t *dst,
                          uint16_t capacity,
                          uint16_t *received)
{
    uint16_t available;
    uint16_t pointer;

    if (received == NULL || dst == NULL) {
        return -1;
    }
    *received = 0U;
    if (w5100_read16_stable(socket_reg(socket, W5100_SN_RX_RSR), &available) != 0) {
        return -1;
    }
    if (available == 0U) {
        return 0;
    }
    if (available > capacity) {
        available = capacity;
    }
    if (w5100_read16(socket_reg(socket, W5100_SN_RX_RD), &pointer) != 0 ||
        socket_ring_read(socket, pointer, dst, available) != 0 ||
        w5100_write16(socket_reg(socket, W5100_SN_RX_RD),
                      (uint16_t)(pointer + available)) != 0 ||
        uthernet2_write_reg(socket_reg(socket, W5100_SN_IR), W5100_IR_RECV) != 0 ||
        socket_command(socket, W5100_CR_RECV) != 0) {
        return -1;
    }
    *received = available;
    return 0;
}

/* Returns 1 when no prior SEND is pending, 0 while it remains in flight,
 * and -1 after a timeout or register error. */
static int socket_send_ready(uint8_t socket, uint8_t *pending)
{
    uint8_t interrupt;

    if (pending == NULL || *pending == 0U) {
        return 1;
    }
    if (uthernet2_read_reg(socket_reg(socket, W5100_SN_IR), &interrupt) != 0) {
        return -1;
    }
    if ((interrupt & W5100_IR_TIMEOUT) != 0U) {
        (void)uthernet2_write_reg(socket_reg(socket, W5100_SN_IR),
                                 W5100_IR_TIMEOUT);
        *pending = 0U;
        return -1;
    }
    if ((interrupt & W5100_IR_SENDOK) == 0U) {
        return 0;
    }
    (void)uthernet2_write_reg(socket_reg(socket, W5100_SN_IR), W5100_IR_SENDOK);
    *pending = 0U;
    return 1;
}

static int socket_send_start(uint8_t socket,
                             const uint8_t *src,
                             uint16_t len,
                             uint8_t *pending)
{
    uint16_t free_size;
    uint16_t pointer;

    if (src == NULL || len == 0U || pending == NULL || *pending != 0U ||
        w5100_read16_stable(socket_reg(socket, W5100_SN_TX_FSR), &free_size) != 0) {
        return -1;
    }
    if (free_size < len) {
        return 0;
    }
    if (w5100_read16(socket_reg(socket, W5100_SN_TX_WR), &pointer) != 0 ||
        socket_ring_write(socket, pointer, src, len) != 0 ||
        w5100_write16(socket_reg(socket, W5100_SN_TX_WR),
                      (uint16_t)(pointer + len)) != 0 ||
        uthernet2_write_reg(socket_reg(socket, W5100_SN_IR),
                           W5100_IR_SENDOK | W5100_IR_TIMEOUT) != 0 ||
        socket_command(socket, W5100_CR_SEND) != 0) {
        return -1;
    }
    *pending = 1U;
    return 1;
}

static uint8_t ip_is_nonzero(const uint8_t ip[UTHERNET2_IPV4_LEN])
{
    return (uint8_t)((ip[0] | ip[1] | ip[2] | ip[3]) != 0U);
}

static uint8_t peer_on_local_subnet(const uint8_t peer[UTHERNET2_IPV4_LEN])
{
    if (peer == NULL || ip_is_nonzero(g_ftp.network.subnet) == 0U) {
        return 0U;
    }
    for (uint8_t i = 0U; i < UTHERNET2_IPV4_LEN; ++i) {
        if ((peer[i] & g_ftp.network.subnet[i]) !=
            (g_ftp.network.ip[i] & g_ftp.network.subnet[i])) {
            return 0U;
        }
    }
    return 1U;
}

static int socket_peer(uint8_t socket, uint8_t peer[UTHERNET2_IPV4_LEN])
{
    return uthernet2_read_block(socket_reg(socket, W5100_SN_DIPR),
                                peer,
                                UTHERNET2_IPV4_LEN);
}

static void set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len != 0U) {
        (void)snprintf(detail, detail_len, "%s", text != NULL ? text : "");
    }
}

static int ftp_reply(const char *fmt, ...)
{
    va_list args;
    int len;

    if (g_ftp.control_out_len != 0U) {
        return -1;
    }
    va_start(args, fmt);
    len = vsnprintf(g_ftp.control_out, sizeof(g_ftp.control_out), fmt, args);
    va_end(args);
    if (len < 0 || (size_t)len >= sizeof(g_ftp.control_out)) {
        g_ftp.control_out_len = 0U;
        return -1;
    }
    g_ftp.control_out_len = (uint16_t)len;
    return 0;
}

static int ftp_close_file_objects(void)
{
    int failed = 0;

    if (g_ftp.file_open != 0U) {
        if (g_ftp.transfer == FTP_TRANSFER_STOR &&
            f_sync(&g_ftp.file) != FR_OK) {
            failed = 1;
        }
        if (f_close(&g_ftp.file) != FR_OK) {
            failed = 1;
        }
        g_ftp.file_open = 0U;
    }
    if (g_ftp.dir_open != 0U) {
        if (f_closedir(&g_ftp.dir) != FR_OK) {
            failed = 1;
        }
        g_ftp.dir_open = 0U;
    }
    return failed != 0 ? -1 : 0;
}

static void ftp_abort_transfer(void)
{
    const uint8_t changed = g_ftp.write_changed;

    (void)ftp_close_file_objects();
    socket_close(FTP_DATA_SOCKET);
    g_ftp.data_listening = 0U;
    g_ftp.data_connected = 0U;
    g_ftp.data_tx_pending = 0U;
    g_ftp.data_buffer_len = 0U;
    g_ftp.transfer_eof = 0U;
    g_ftp.transfer = FTP_TRANSFER_NONE;
    g_ftp.write_changed = 0U;
    if (changed != 0U) {
        screenshot_service_note_local_sd_write_complete();
    }
}

static void ftp_finish_transfer(uint8_t success)
{
    const uint8_t changed = g_ftp.write_changed;

    if (ftp_close_file_objects() != 0) {
        success = 0U;
    }
    socket_disconnect(FTP_DATA_SOCKET);
    g_ftp.data_listening = 0U;
    g_ftp.data_connected = 0U;
    g_ftp.data_tx_pending = 0U;
    g_ftp.data_buffer_len = 0U;
    g_ftp.transfer_eof = 0U;
    g_ftp.transfer = FTP_TRANSFER_NONE;
    g_ftp.write_changed = 0U;
    if (changed != 0U) {
        screenshot_service_note_local_sd_write_complete();
    }
    (void)ftp_reply(success != 0U ?
                    "226 Transfer complete.\r\n" :
                    "426 Transfer aborted.\r\n");
}

static int normalize_virtual_path(const char *input,
                                  char output[FTP_PATH_LEN])
{
    size_t len;
    const char *p;

    if (output == NULL) {
        return -1;
    }
    if (input == NULL || input[0] == '\0') {
        (void)snprintf(output, FTP_PATH_LEN, "%s", g_ftp.cwd);
        return 0;
    }
    if (input[0] == '/' || input[0] == '\\') {
        output[0] = '/';
        output[1] = '\0';
        len = 1U;
    } else {
        len = strlen(g_ftp.cwd);
        if (len >= FTP_PATH_LEN) {
            return -1;
        }
        memcpy(output, g_ftp.cwd, len + 1U);
    }

    p = input;
    while (*p != '\0') {
        const char *segment;
        size_t segment_len;

        while (*p == '/' || *p == '\\') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        segment = p;
        while (*p != '\0' && *p != '/' && *p != '\\') {
            if (*p == ':' || (unsigned char)*p < 0x20U) {
                return -1;
            }
            ++p;
        }
        segment_len = (size_t)(p - segment);
        if (segment_len == 1U && segment[0] == '.') {
            continue;
        }
        if (segment_len == 2U && segment[0] == '.' && segment[1] == '.') {
            if (len > 1U) {
                while (len > 1U && output[len - 1U] != '/') {
                    --len;
                }
                if (len > 1U) {
                    --len;
                }
                output[len] = '\0';
            }
            continue;
        }
        if (segment_len == 0U ||
            len + (len > 1U ? 1U : 0U) + segment_len >= FTP_PATH_LEN) {
            return -1;
        }
        if (len > 1U) {
            output[len++] = '/';
        }
        memcpy(&output[len], segment, segment_len);
        len += segment_len;
        output[len] = '\0';
    }
    return 0;
}

static int make_fat_path(const char *input,
                         char virtual_path[FTP_PATH_LEN],
                         char fat_path[FTP_FAT_PATH_LEN])
{
    int len;

    if (normalize_virtual_path(input, virtual_path) != 0) {
        return -1;
    }
    len = snprintf(fat_path, FTP_FAT_PATH_LEN, "0:%s", virtual_path);
    if (len < 0 || (size_t)len >= FTP_FAT_PATH_LEN) {
        return -1;
    }
    return 0;
}

static uint8_t data_socket_available(void)
{
    uint8_t status;

    if (socket_status(FTP_DATA_SOCKET, &status) != 0) {
        return 0U;
    }
    return (uint8_t)(status == W5100_SR_LISTEN ||
                     status == W5100_SR_ESTABLISHED);
}

static int ftp_open_passive(void)
{
    ftp_abort_transfer();
    if (socket_listen(FTP_DATA_SOCKET, FTP_PASSIVE_PORT) != 0) {
        return -1;
    }
    g_ftp.data_listening = 1U;
    return 0;
}

static int ftp_prepare_transfer(ftp_transfer_t transfer, const char *argument)
{
    char virtual_path[FTP_PATH_LEN];
    char fat_path[FTP_FAT_PATH_LEN];
    FRESULT fr;

    if (data_socket_available() == 0U) {
        (void)ftp_reply("425 Use PASV or EPSV first.\r\n");
        return -1;
    }
    if (make_fat_path(argument, virtual_path, fat_path) != 0) {
        (void)ftp_reply("550 Invalid path.\r\n");
        return -1;
    }
    if (transfer == FTP_TRANSFER_LIST || transfer == FTP_TRANSFER_NLST) {
        fr = f_opendir(&g_ftp.dir, fat_path);
        if (fr != FR_OK) {
            (void)ftp_reply("550 Cannot open directory.\r\n");
            return -1;
        }
        g_ftp.dir_open = 1U;
    } else if (transfer == FTP_TRANSFER_RETR) {
        fr = f_open(&g_ftp.file, fat_path, FA_READ);
        if (fr != FR_OK) {
            (void)ftp_reply("550 Cannot open file.\r\n");
            return -1;
        }
        g_ftp.file_open = 1U;
    } else if (transfer == FTP_TRANSFER_STOR) {
        fr = f_open(&g_ftp.file, fat_path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            (void)ftp_reply("550 Cannot create file.\r\n");
            return -1;
        }
        g_ftp.file_open = 1U;
        g_ftp.write_changed = 1U;
    }
    g_ftp.transfer = transfer;
    g_ftp.transfer_eof = 0U;
    (void)ftp_reply("150 Opening passive data connection.\r\n");
    return 0;
}

static void command_upper(char *command)
{
    while (command != NULL && *command != '\0') {
        if (*command >= 'a' && *command <= 'z') {
            *command = (char)(*command - ('a' - 'A'));
        }
        ++command;
    }
}

static void ftp_handle_command(char *line)
{
    char *argument;
    char virtual_path[FTP_PATH_LEN];
    char fat_path[FTP_FAT_PATH_LEN];
    FILINFO info;
    FRESULT fr;

    argument = strchr(line, ' ');
    if (argument != NULL) {
        *argument++ = '\0';
        while (*argument == ' ') {
            ++argument;
        }
    } else {
        argument = &line[strlen(line)];
    }
    command_upper(line);

    if (strcmp(line, "USER") == 0) {
        g_ftp.logged_in = 0U;
        (void)ftp_reply("331 Anonymous login; any password is accepted.\r\n");
        return;
    }
    if (strcmp(line, "PASS") == 0) {
        g_ftp.logged_in = 1U;
        (void)ftp_reply("230 Anonymous read/write access granted.\r\n");
        return;
    }
    if (strcmp(line, "QUIT") == 0) {
        ftp_abort_transfer();
        (void)ftp_reply("221 Goodbye.\r\n");
        g_ftp.close_control_after_tx = 1U;
        return;
    }
    if (g_ftp.logged_in == 0U) {
        (void)ftp_reply("530 Log in with USER and PASS first.\r\n");
        return;
    }

    if (strcmp(line, "SYST") == 0) {
        (void)ftp_reply("215 UNIX Type: L8\r\n");
    } else if (strcmp(line, "FEAT") == 0) {
        (void)ftp_reply("211-Features:\r\n EPSV\r\n MDTM\r\n PASV\r\n SIZE\r\n211 End\r\n");
    } else if (strcmp(line, "OPTS") == 0) {
        (void)ftp_reply("200 Option accepted.\r\n");
    } else if (strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 ||
               strcmp(line, "STRU") == 0) {
        (void)ftp_reply("200 Transfer setting accepted.\r\n");
    } else if (strcmp(line, "NOOP") == 0) {
        (void)ftp_reply("200 NOOP ok.\r\n");
    } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
        (void)ftp_reply("257 \"%s\" is the current directory.\r\n", g_ftp.cwd);
    } else if (strcmp(line, "CWD") == 0 || strcmp(line, "XCWD") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            (strcmp(virtual_path, "/") != 0 &&
             (f_stat(fat_path, &info) != FR_OK ||
              (info.fattrib & AM_DIR) == 0U))) {
            (void)ftp_reply("550 Directory unavailable.\r\n");
        } else {
            (void)snprintf(g_ftp.cwd, sizeof(g_ftp.cwd), "%s", virtual_path);
            (void)ftp_reply("250 Directory changed.\r\n");
        }
    } else if (strcmp(line, "CDUP") == 0 || strcmp(line, "XCUP") == 0) {
        if (normalize_virtual_path("..", virtual_path) == 0) {
            (void)snprintf(g_ftp.cwd, sizeof(g_ftp.cwd), "%s", virtual_path);
        }
        (void)ftp_reply("250 Directory changed.\r\n");
    } else if (strcmp(line, "PASV") == 0) {
        if (ftp_open_passive() != 0) {
            (void)ftp_reply("425 Cannot open passive socket.\r\n");
        } else {
            (void)ftp_reply("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
                            (unsigned)g_ftp.network.ip[0],
                            (unsigned)g_ftp.network.ip[1],
                            (unsigned)g_ftp.network.ip[2],
                            (unsigned)g_ftp.network.ip[3],
                            (unsigned)(FTP_PASSIVE_PORT >> 8U),
                            (unsigned)(FTP_PASSIVE_PORT & 0xFFU));
        }
    } else if (strcmp(line, "EPSV") == 0) {
        if (ftp_open_passive() != 0) {
            (void)ftp_reply("425 Cannot open passive socket.\r\n");
        } else {
            (void)ftp_reply("229 Entering Extended Passive Mode (|||%u|).\r\n",
                            (unsigned)FTP_PASSIVE_PORT);
        }
    } else if (strcmp(line, "PORT") == 0 || strcmp(line, "EPRT") == 0) {
        (void)ftp_reply("502 Active mode is disabled; use PASV or EPSV.\r\n");
    } else if (strcmp(line, "LIST") == 0) {
        if (argument[0] == '-') {
            argument = &argument[strlen(argument)];
        }
        (void)ftp_prepare_transfer(FTP_TRANSFER_LIST, argument);
    } else if (strcmp(line, "NLST") == 0) {
        (void)ftp_prepare_transfer(FTP_TRANSFER_NLST, argument);
    } else if (strcmp(line, "RETR") == 0) {
        (void)ftp_prepare_transfer(FTP_TRANSFER_RETR, argument);
    } else if (strcmp(line, "STOR") == 0) {
        (void)ftp_prepare_transfer(FTP_TRANSFER_STOR, argument);
    } else if (strcmp(line, "ABOR") == 0) {
        ftp_abort_transfer();
        (void)ftp_reply("226 Abort complete.\r\n");
    } else if (strcmp(line, "SIZE") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK || (info.fattrib & AM_DIR) != 0U) {
            (void)ftp_reply("550 File unavailable.\r\n");
        } else {
            (void)ftp_reply("213 %lu\r\n", (unsigned long)info.fsize);
        }
    } else if (strcmp(line, "MDTM") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK) {
            (void)ftp_reply("550 File unavailable.\r\n");
        } else {
            (void)ftp_reply("213 %04u%02u%02u%02u%02u%02u\r\n",
                            (unsigned)(1980U + (info.fdate >> 9U)),
                            (unsigned)((info.fdate >> 5U) & 0x0FU),
                            (unsigned)(info.fdate & 0x1FU),
                            (unsigned)(info.ftime >> 11U),
                            (unsigned)((info.ftime >> 5U) & 0x3FU),
                            (unsigned)((info.ftime & 0x1FU) * 2U));
        }
    } else if (strcmp(line, "DELE") == 0 || strcmp(line, "RMD") == 0 ||
               strcmp(line, "XRMD") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            f_unlink(fat_path) != FR_OK) {
            (void)ftp_reply("550 Delete failed.\r\n");
        } else {
            screenshot_service_note_local_sd_write_complete();
            (void)ftp_reply("250 Deleted.\r\n");
        }
    } else if (strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            f_mkdir(fat_path) != FR_OK) {
            (void)ftp_reply("550 Create directory failed.\r\n");
        } else {
            screenshot_service_note_local_sd_write_complete();
            (void)ftp_reply("257 \"%s\" created.\r\n", virtual_path);
        }
    } else if (strcmp(line, "RNFR") == 0) {
        if (make_fat_path(argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK) {
            g_ftp.rename_from[0] = '\0';
            (void)ftp_reply("550 Source unavailable.\r\n");
        } else {
            (void)snprintf(g_ftp.rename_from, sizeof(g_ftp.rename_from),
                           "%s", virtual_path);
            (void)ftp_reply("350 Send RNTO.\r\n");
        }
    } else if (strcmp(line, "RNTO") == 0) {
        char old_fat[FTP_FAT_PATH_LEN];

        if (g_ftp.rename_from[0] == '\0' ||
            make_fat_path(argument, virtual_path, fat_path) != 0 ||
            snprintf(old_fat, sizeof(old_fat), "0:%s", g_ftp.rename_from) < 0) {
            (void)ftp_reply("503 Send RNFR first.\r\n");
        } else {
            fr = f_rename(old_fat, fat_path);
            g_ftp.rename_from[0] = '\0';
            if (fr != FR_OK) {
                (void)ftp_reply("550 Rename failed.\r\n");
            } else {
                screenshot_service_note_local_sd_write_complete();
                (void)ftp_reply("250 Renamed.\r\n");
            }
        }
    } else if (strcmp(line, "STAT") == 0) {
        (void)ftp_reply("211 Appletini ONE anonymous FTP ready.\r\n");
    } else if (strcmp(line, "AUTH") == 0) {
        (void)ftp_reply("534 TLS is not available.\r\n");
    } else {
        (void)ftp_reply("502 Command not implemented.\r\n");
    }
}

static void ftp_control_output_poll(void)
{
    int ready;

    if (g_ftp.control_connected == 0U) {
        return;
    }
    ready = socket_send_ready(FTP_CONTROL_SOCKET, &g_ftp.control_tx_pending);
    if (ready < 0) {
        socket_close(FTP_CONTROL_SOCKET);
        g_ftp.control_connected = 0U;
        return;
    }
    if (ready == 0) {
        return;
    }
    if (g_ftp.control_out_len != 0U) {
        const int sent = socket_send_start(FTP_CONTROL_SOCKET,
                                           (const uint8_t *)g_ftp.control_out,
                                           g_ftp.control_out_len,
                                           &g_ftp.control_tx_pending);
        if (sent < 0) {
            socket_close(FTP_CONTROL_SOCKET);
            g_ftp.control_connected = 0U;
        } else if (sent > 0) {
            g_ftp.control_out_len = 0U;
        }
        return;
    }
    if (g_ftp.close_control_after_tx != 0U) {
        (void)socket_command(FTP_CONTROL_SOCKET, W5100_CR_DISCON);
        socket_close(FTP_CONTROL_SOCKET);
        g_ftp.control_connected = 0U;
        g_ftp.close_control_after_tx = 0U;
    }
}

static void ftp_control_input_poll(void)
{
    uint16_t received;
    char *newline;
    size_t line_len;

    if (g_ftp.control_connected == 0U || g_ftp.control_out_len != 0U ||
        g_ftp.control_tx_pending != 0U || g_ftp.close_control_after_tx != 0U) {
        return;
    }
    if (g_ftp.control_rx_len < sizeof(g_ftp.control_rx) - 1U) {
        if (socket_receive(FTP_CONTROL_SOCKET,
                           (uint8_t *)&g_ftp.control_rx[g_ftp.control_rx_len],
                           (uint16_t)(sizeof(g_ftp.control_rx) -
                                      g_ftp.control_rx_len - 1U),
                           &received) != 0) {
            socket_close(FTP_CONTROL_SOCKET);
            g_ftp.control_connected = 0U;
            return;
        }
        g_ftp.control_rx_len = (uint16_t)(g_ftp.control_rx_len + received);
        g_ftp.control_rx[g_ftp.control_rx_len] = '\0';
    }
    newline = strchr(g_ftp.control_rx, '\n');
    if (newline == NULL) {
        if (g_ftp.control_rx_len == sizeof(g_ftp.control_rx) - 1U) {
            g_ftp.control_rx_len = 0U;
            g_ftp.control_rx[0] = '\0';
            (void)ftp_reply("500 Command too long.\r\n");
        }
        return;
    }
    line_len = (size_t)(newline - g_ftp.control_rx);
    if (line_len != 0U && g_ftp.control_rx[line_len - 1U] == '\r') {
        --line_len;
    }
    g_ftp.control_rx[line_len] = '\0';
    ftp_handle_command(g_ftp.control_rx);
    {
        const size_t consumed = (size_t)(newline - g_ftp.control_rx) + 1U;
        const size_t remaining = g_ftp.control_rx_len - consumed;

        memmove(g_ftp.control_rx, newline + 1, remaining);
        g_ftp.control_rx_len = (uint16_t)remaining;
        g_ftp.control_rx[remaining] = '\0';
    }
}

static void ftp_control_poll(void)
{
    uint8_t status;

    if (socket_status(FTP_CONTROL_SOCKET, &status) != 0) {
        return;
    }
    if (status == W5100_SR_CLOSED) {
        ftp_abort_transfer();
        g_ftp.control_connected = 0U;
        g_ftp.control_tx_pending = 0U;
        g_ftp.control_out_len = 0U;
        g_ftp.control_rx_len = 0U;
        g_ftp.close_control_after_tx = 0U;
        (void)socket_listen(FTP_CONTROL_SOCKET, FTP_CONTROL_PORT);
        return;
    }
    if (status == W5100_SR_CLOSE_WAIT) {
        ftp_abort_transfer();
        socket_close(FTP_CONTROL_SOCKET);
        g_ftp.control_connected = 0U;
        return;
    }
    if (status != W5100_SR_ESTABLISHED) {
        return;
    }
    if (g_ftp.control_connected == 0U) {
        uint8_t peer[UTHERNET2_IPV4_LEN];

        g_ftp.control_connected = 1U;
        g_ftp.logged_in = 0U;
        g_ftp.control_rx_len = 0U;
        g_ftp.control_out_len = 0U;
        g_ftp.control_tx_pending = 0U;
        g_ftp.close_control_after_tx = 0U;
        (void)snprintf(g_ftp.cwd, sizeof(g_ftp.cwd), "/");
        g_ftp.rename_from[0] = '\0';
        if (socket_peer(FTP_CONTROL_SOCKET, peer) != 0 ||
            peer_on_local_subnet(peer) == 0U) {
            (void)ftp_reply("421 Appletini FTP accepts local-subnet clients only.\r\n");
            g_ftp.close_control_after_tx = 1U;
        } else {
            memcpy(g_ftp.control_peer, peer, sizeof(g_ftp.control_peer));
            (void)ftp_reply("220 Appletini ONE anonymous FTP ready.\r\n");
        }
    }
    ftp_control_output_poll();
    ftp_control_input_poll();
    ftp_control_output_poll();
}

static void ftp_data_accept_poll(void)
{
    uint8_t status;
    uint8_t peer[UTHERNET2_IPV4_LEN];

    if (g_ftp.data_listening == 0U || g_ftp.data_connected != 0U ||
        socket_status(FTP_DATA_SOCKET, &status) != 0 ||
        status != W5100_SR_ESTABLISHED) {
        return;
    }
    if (socket_peer(FTP_DATA_SOCKET, peer) != 0 ||
        peer_on_local_subnet(peer) == 0U ||
        memcmp(peer, g_ftp.control_peer, sizeof(peer)) != 0) {
        ftp_abort_transfer();
        (void)ftp_reply("425 Data connection must come from the control client.\r\n");
        return;
    }
    g_ftp.data_connected = 1U;
    g_ftp.data_listening = 0U;
}

static int ftp_format_list_line(const FILINFO *info,
                                uint8_t names_only,
                                uint8_t *out,
                                size_t out_len)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const unsigned month = (unsigned)((info->fdate >> 5U) & 0x0FU);
    const unsigned day = (unsigned)(info->fdate & 0x1FU);
    const unsigned hour = (unsigned)(info->ftime >> 11U);
    const unsigned minute = (unsigned)((info->ftime >> 5U) & 0x3FU);

    if (names_only != 0U) {
        return snprintf((char *)out, out_len, "%s\r\n", info->fname);
    }
    return snprintf((char *)out,
                    out_len,
                    "%s 1 ftp ftp %10lu %s %02u %02u:%02u %s\r\n",
                    (info->fattrib & AM_DIR) != 0U ?
                        "drwxrwxrwx" : "-rw-rw-rw-",
                    (unsigned long)info->fsize,
                    (month >= 1U && month <= 12U) ? months[month - 1U] : "Jan",
                    day,
                    hour,
                    minute,
                    info->fname);
}

static void ftp_send_transfer_poll(void)
{
    int ready;
    UINT bytes_read;

    ready = socket_send_ready(FTP_DATA_SOCKET, &g_ftp.data_tx_pending);
    if (ready < 0) {
        ftp_finish_transfer(0U);
        return;
    }
    if (ready == 0) {
        return;
    }
    if (g_ftp.transfer_eof != 0U) {
        ftp_finish_transfer(1U);
        return;
    }
    if (g_ftp.data_buffer_len != 0U) {
        const int sent = socket_send_start(FTP_DATA_SOCKET,
                                           g_ftp.data_buffer,
                                           g_ftp.data_buffer_len,
                                           &g_ftp.data_tx_pending);

        if (sent < 0) {
            ftp_finish_transfer(0U);
        } else if (sent > 0) {
            g_ftp.data_buffer_len = 0U;
        }
        return;
    }

    if (g_ftp.transfer == FTP_TRANSFER_RETR) {
        if (f_read(&g_ftp.file,
                   g_ftp.data_buffer,
                   sizeof(g_ftp.data_buffer),
                   &bytes_read) != FR_OK) {
            ftp_finish_transfer(0U);
            return;
        }
        if (bytes_read == 0U) {
            g_ftp.transfer_eof = 1U;
            ftp_finish_transfer(1U);
            return;
        }
        g_ftp.data_buffer_len = (uint16_t)bytes_read;
        {
            const int sent = socket_send_start(FTP_DATA_SOCKET,
                                               g_ftp.data_buffer,
                                               g_ftp.data_buffer_len,
                                               &g_ftp.data_tx_pending);
            if (sent < 0) {
                ftp_finish_transfer(0U);
            } else if (sent > 0) {
                g_ftp.data_buffer_len = 0U;
            }
        }
        return;
    }

    if (g_ftp.transfer == FTP_TRANSFER_LIST ||
        g_ftp.transfer == FTP_TRANSFER_NLST) {
        FILINFO info;
        int len;

        if (f_readdir(&g_ftp.dir, &info) != FR_OK) {
            ftp_finish_transfer(0U);
            return;
        }
        if (info.fname[0] == '\0') {
            g_ftp.transfer_eof = 1U;
            ftp_finish_transfer(1U);
            return;
        }
        len = ftp_format_list_line(&info,
                                   (uint8_t)(g_ftp.transfer == FTP_TRANSFER_NLST),
                                   g_ftp.data_buffer,
                                   sizeof(g_ftp.data_buffer));
        if (len <= 0 || (size_t)len >= sizeof(g_ftp.data_buffer)) {
            ftp_finish_transfer(0U);
            return;
        }
        g_ftp.data_buffer_len = (uint16_t)len;
        {
            const int sent = socket_send_start(FTP_DATA_SOCKET,
                                               g_ftp.data_buffer,
                                               g_ftp.data_buffer_len,
                                               &g_ftp.data_tx_pending);
            if (sent < 0) {
                ftp_finish_transfer(0U);
            } else if (sent > 0) {
                g_ftp.data_buffer_len = 0U;
            }
        }
    }
}

static void ftp_store_transfer_poll(void)
{
    uint8_t status;
    uint16_t received;
    UINT written;

    if (socket_receive(FTP_DATA_SOCKET,
                       g_ftp.data_buffer,
                       sizeof(g_ftp.data_buffer),
                       &received) != 0) {
        ftp_finish_transfer(0U);
        return;
    }
    if (received != 0U) {
        if (f_write(&g_ftp.file, g_ftp.data_buffer, received, &written) != FR_OK ||
            written != received) {
            ftp_finish_transfer(0U);
        }
        return;
    }
    if (socket_status(FTP_DATA_SOCKET, &status) != 0) {
        ftp_finish_transfer(0U);
    } else if (status == W5100_SR_CLOSE_WAIT || status == W5100_SR_CLOSED) {
        ftp_finish_transfer(1U);
    }
}

static void ftp_data_poll(void)
{
    uint8_t status;

    ftp_data_accept_poll();
    if (g_ftp.transfer == FTP_TRANSFER_NONE || g_ftp.data_connected == 0U) {
        return;
    }
    if (socket_status(FTP_DATA_SOCKET, &status) != 0 ||
        (status != W5100_SR_ESTABLISHED && status != W5100_SR_CLOSE_WAIT)) {
        ftp_finish_transfer(0U);
        return;
    }
    if (g_ftp.transfer == FTP_TRANSFER_STOR) {
        ftp_store_transfer_poll();
    } else if (status == W5100_SR_CLOSE_WAIT) {
        ftp_finish_transfer(0U);
    } else {
        ftp_send_transfer_poll();
    }
}

int ftp_sd_service_start(char *detail, size_t detail_len)
{
    FRESULT fr;

    if (g_ftp.active != 0U) {
        set_detail(detail, detail_len, "FTP ALREADY ACTIVE");
        return 0;
    }
    memset(&g_ftp, 0, sizeof(g_ftp));
    if (uthernet2_read_network_config(&g_ftp.network) != 0 ||
        ip_is_nonzero(g_ftp.network.ip) == 0U ||
        ip_is_nonzero(g_ftp.network.subnet) == 0U) {
        set_detail(detail, detail_len, "SET A VALID IP AND SUBNET FIRST");
        return -1;
    }
    fr = f_mount(&g_ftp.fs, "0:/", 1U);
    if (fr != FR_OK) {
        set_detail(detail, detail_len, "FTP COULD NOT MOUNT SD CARD");
        return -(int)fr;
    }
    for (uint8_t socket = 0U; socket < 4U; ++socket) {
        socket_close(socket);
    }
    if (uthernet2_write_reg(W5100_REG_RMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        uthernet2_write_reg(W5100_REG_TMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        socket_listen(FTP_CONTROL_SOCKET, FTP_CONTROL_PORT) != 0) {
        (void)f_mount((FATFS *)0, "0:/", 0U);
        set_detail(detail, detail_len, "FTP COULD NOT OPEN TCP PORT 21");
        return -1;
    }
    (void)snprintf(g_ftp.cwd, sizeof(g_ftp.cwd), "/");
    g_ftp.active = 1U;
    if (detail != NULL && detail_len != 0U) {
        (void)snprintf(detail,
                       detail_len,
                       "FTP %u.%u.%u.%u ANON R/W - ENTER/ESC EXITS",
                       (unsigned)g_ftp.network.ip[0],
                       (unsigned)g_ftp.network.ip[1],
                       (unsigned)g_ftp.network.ip[2],
                       (unsigned)g_ftp.network.ip[3]);
    }
    return 0;
}

void ftp_sd_service_stop(void)
{
    if (g_ftp.active == 0U) {
        return;
    }
    ftp_abort_transfer();
    socket_close(FTP_CONTROL_SOCKET);
    (void)f_mount((FATFS *)0, "0:/", 0U);
    memset(&g_ftp, 0, sizeof(g_ftp));
}

void ftp_sd_service_poll(void)
{
    if (g_ftp.active == 0U) {
        return;
    }
    ftp_control_poll();
    ftp_data_poll();
    ftp_control_output_poll();
}

uint8_t ftp_sd_service_active(void)
{
    return g_ftp.active;
}
