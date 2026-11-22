#include "ftp_sd_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "screenshot_service.h"
#include "uthernet2_control.h"
#include "http_seed.h"
#include "xiltimer.h"

/* The W5100S has four sockets. The service shares them between three
 * control connections at most and the data connections. One socket stays
 * free for data when three clients are connected. */
#define FTP_SOCKET_COUNT         4U
#define FTP_SESSION_COUNT        3U
#define FTP_NO_SOCKET            0xFFU
#define FTP_CONTROL_PORT         21U
#define FTP_ACTIVE_PORT          20U
#define FTP_PASSIVE_PORT         50000U
#define FTP_PASSIVE_PORT_COUNT   1000U
#define FTP_CONTROL_BUFFER_LEN   512U
#define FTP_CONTROL_OUT_LEN      768U
#define FTP_DATA_BUFFER_LEN      1024U
#define FTP_STORE_BUFFER_LEN     32768U
#define FTP_STORE_RECEIVE_MAX    8192U
#define FTP_PATH_LEN             256U
#define FTP_FAT_PATH_LEN         (FTP_PATH_LEN + 3U)
#define FTP_LINGER_MS            3000U
#define FTP_WAIT_NOTE_MS         5000U
#define FTP_SN_DPORT             0x10U
#define FTP_CR_CONNECT           0x04U

typedef enum {
    FTP_TRANSFER_NONE = 0,
    FTP_TRANSFER_LIST,
    FTP_TRANSFER_NLST,
    FTP_TRANSFER_RETR,
    FTP_TRANSFER_STOR
} ftp_transfer_t;

typedef struct {
    uint8_t used;
    uint8_t control_socket;
    uint8_t data_socket;
    uint8_t control_tx_pending;
    uint8_t close_control_after_tx;
    uint8_t logged_in;
    uint8_t data_listening;
    uint8_t data_connecting;
    uint8_t data_connected;
    uint8_t data_tx_pending;
    uint8_t file_open;
    uint8_t dir_open;
    uint8_t write_changed;
    uint8_t active_mode;        /* PORT or EPRT gave an address */
    uint8_t passive_wait;       /* 227 or 229 reply waits for a socket */
    uint8_t stor_append;
    ftp_transfer_t transfer;
    FIL file;
    DIR dir;
    uint8_t control_peer[UTHERNET2_IPV4_LEN];
    uint16_t active_port;
    uint16_t data_port;
    uint32_t restart_offset;    /* from REST, for the next command only */
    uint32_t stor_offset;
    uint64_t wait_note_ms;
    char cwd[FTP_PATH_LEN];
    char rename_from[FTP_PATH_LEN];
    char pending_stor_path[FTP_FAT_PATH_LEN];
    char control_rx[FTP_CONTROL_BUFFER_LEN];
    uint16_t control_rx_len;
    char control_out[FTP_CONTROL_OUT_LEN];
    uint16_t control_out_len;
    uint8_t data_buffer[FTP_DATA_BUFFER_LEN] __attribute__((aligned(32)));
    uint16_t data_buffer_len;
    /* File data moves between the SD card and this buffer 32 KiB at a
     * time: small reads and writes are slow on the SD card. */
    uint8_t store_buffer[FTP_STORE_BUFFER_LEN] __attribute__((aligned(32)));
    uint32_t store_len;
    uint32_t send_pos;          /* download: bytes of store_buffer sent */
} ftp_session_t;

typedef struct {
    uint8_t active;
    uint8_t listen_socket;
    uint8_t linger[FTP_SOCKET_COUNT];
    uint16_t next_passive;
    uint64_t linger_since[FTP_SOCKET_COUNT];
    FATFS fs;
    uthernet2_network_config_t network;
    ftp_session_t sessions[FTP_SESSION_COUNT];
} ftp_sd_state_t;

static ftp_sd_state_t g_ftp;

static void socket_close(uint8_t socket)
{
    (void)uthernet2_w5100_socket_command(socket, W5100_CR_CLOSE);
    (void)uthernet2_write_reg(
        uthernet2_w5100_socket_reg(socket, W5100_SN_IR), 0xFFU);
}

/* Complete the TCP close handshake so clients see EOF on a finished data
 * transfer. W5100 CLOSE discards the socket immediately and does not send the
 * FIN that FTP clients wait for after the final payload byte. */
static void socket_disconnect(uint8_t socket)
{
    uint8_t status;

    if (uthernet2_w5100_socket_status(socket, &status) == 0 &&
        (status == W5100_SR_ESTABLISHED || status == W5100_SR_CLOSE_WAIT) &&
        uthernet2_w5100_socket_command(socket, W5100_CR_DISCON) == 0) {
        return;
    }
    socket_close(socket);
}

static int socket_listen(uint8_t socket, uint16_t port)
{
    uint8_t status;

    socket_close(socket);
    if (uthernet2_write_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_MR),
            W5100_SN_MR_TCP) != 0 ||
        uthernet2_w5100_write16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_PORT), port) != 0 ||
        uthernet2_w5100_socket_command(socket, W5100_CR_OPEN) != 0 ||
        uthernet2_w5100_socket_status(socket, &status) != 0 ||
        status != W5100_SR_INIT ||
        uthernet2_w5100_socket_command(socket, W5100_CR_LISTEN) != 0 ||
        uthernet2_w5100_socket_status(socket, &status) != 0 ||
        status != W5100_SR_LISTEN) {
        socket_close(socket);
        return -1;
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
    if (uthernet2_w5100_read16_stable(
            uthernet2_w5100_socket_reg(socket, W5100_SN_RX_RSR),
            &available) != 0) {
        return -1;
    }
    if (available == 0U) {
        return 0;
    }
    if (available > capacity) {
        available = capacity;
    }
    if (uthernet2_w5100_read16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_RX_RD),
            &pointer) != 0 ||
        uthernet2_w5100_socket_ring_read(
            socket, pointer, dst, available) != 0 ||
        uthernet2_w5100_write16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_RX_RD),
            (uint16_t)(pointer + available)) != 0 ||
        uthernet2_write_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_IR),
            W5100_IR_RECV) != 0 ||
        uthernet2_w5100_socket_command(socket, W5100_CR_RECV) != 0) {
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
    if (uthernet2_read_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_IR),
            &interrupt) != 0) {
        return -1;
    }
    if ((interrupt & W5100_IR_TIMEOUT) != 0U) {
        (void)uthernet2_write_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_IR),
            W5100_IR_TIMEOUT);
        *pending = 0U;
        return -1;
    }
    if ((interrupt & W5100_IR_SENDOK) == 0U) {
        return 0;
    }
    (void)uthernet2_write_reg(
        uthernet2_w5100_socket_reg(socket, W5100_SN_IR), W5100_IR_SENDOK);
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
        uthernet2_w5100_read16_stable(
            uthernet2_w5100_socket_reg(socket, W5100_SN_TX_FSR),
            &free_size) != 0) {
        return -1;
    }
    if (free_size < len) {
        return 0;
    }
    if (uthernet2_w5100_read16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_TX_WR),
            &pointer) != 0 ||
        uthernet2_w5100_socket_ring_write(socket, pointer, src, len) != 0 ||
        uthernet2_w5100_write16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_TX_WR),
            (uint16_t)(pointer + len)) != 0 ||
        uthernet2_write_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_IR),
            W5100_IR_SENDOK | W5100_IR_TIMEOUT) != 0 ||
        uthernet2_w5100_socket_command(socket, W5100_CR_SEND) != 0) {
        return -1;
    }
    *pending = 1U;
    return 1;
}

static int socket_send_buffer(uint8_t socket,
                              const uint8_t *src,
                              uint16_t *len,
                              uint8_t *pending)
{
    int sent;

    if (len == NULL || *len == 0U) {
        return -1;
    }
    sent = socket_send_start(socket, src, *len, pending);
    if (sent > 0) {
        *len = 0U;
    }
    return sent;
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
    return uthernet2_read_block(
        uthernet2_w5100_socket_reg(socket, W5100_SN_DIPR),
        peer,
        UTHERNET2_IPV4_LEN);
}

static void set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len != 0U) {
        (void)snprintf(detail, detail_len, "%s", text != NULL ? text : "");
    }
}

static uint64_t ftp_now_ms(void)
{
    XTime ticks;

    XTime_GetTime(&ticks);
    return (uint64_t)ticks / ((uint64_t)(COUNTS_PER_SECOND) / 1000U);
}

/* Adds a reply to the output of the control connection. A reply that
 * waits for the chip is kept, so a fast transfer cannot lose its 226. */
static int ftp_reply(ftp_session_t *s, const char *fmt, ...)
{
    va_list args;
    int len;
    const size_t room = sizeof(s->control_out) - s->control_out_len;

    va_start(args, fmt);
    len = vsnprintf(&s->control_out[s->control_out_len], room, fmt, args);
    va_end(args);
    if (len < 0 || (size_t)len >= room) {
        s->control_out[s->control_out_len] = '\0';
        return -1;
    }
    s->control_out_len = (uint16_t)(s->control_out_len + (uint16_t)len);
    return 0;
}

static uint8_t socket_owned(uint8_t socket)
{
    if (socket == g_ftp.listen_socket) {
        return 1U;
    }
    for (uint8_t i = 0U; i < FTP_SESSION_COUNT; ++i) {
        const ftp_session_t *s = &g_ftp.sessions[i];

        if (s->used != 0U &&
            (s->control_socket == socket || s->data_socket == socket)) {
            return 1U;
        }
    }
    return 0U;
}

/* Gives a socket back with a TCP close handshake. The socket is not used
 * again until the chip reports it closed, or after FTP_LINGER_MS: a CLOSE
 * command too early would drop the FIN that the client waits for. */
static void socket_release(uint8_t socket)
{
    if (socket >= FTP_SOCKET_COUNT) {
        return;
    }
    socket_disconnect(socket);
    g_ftp.linger[socket] = 1U;
    g_ftp.linger_since[socket] = ftp_now_ms();
}

static uint8_t socket_free(uint8_t socket)
{
    uint8_t status;

    if (socket_owned(socket) != 0U ||
        uthernet2_w5100_socket_status(socket, &status) != 0) {
        return 0U;
    }
    if (status != W5100_SR_CLOSED && g_ftp.linger[socket] != 0U &&
        ftp_now_ms() - g_ftp.linger_since[socket] < FTP_LINGER_MS) {
        return 0U;
    }
    if (status != W5100_SR_CLOSED) {
        socket_close(socket);
    }
    g_ftp.linger[socket] = 0U;
    return 1U;
}

/* Returns a socket for a data connection. It takes the listening control
 * socket when no other socket is free; ftp_listener_poll opens a new one
 * later. */
static uint8_t data_socket_alloc(void)
{
    uint8_t status;

    for (uint8_t socket = 0U; socket < FTP_SOCKET_COUNT; ++socket) {
        if (socket_free(socket) != 0U) {
            return socket;
        }
    }
    if (g_ftp.listen_socket != FTP_NO_SOCKET &&
        uthernet2_w5100_socket_status(g_ftp.listen_socket, &status) == 0 &&
        status == W5100_SR_LISTEN) {
        const uint8_t socket = g_ftp.listen_socket;

        g_ftp.listen_socket = FTP_NO_SOCKET;
        socket_close(socket);
        return socket;
    }
    return FTP_NO_SOCKET;
}

/* Writes the collected upload bytes to the file. */
static int ftp_store_flush(ftp_session_t *s)
{
    UINT written = 0U;
    const UINT len = (UINT)s->store_len;

    s->store_len = 0U;
    if (len == 0U) {
        return 0;
    }
    return (f_write(&s->file, s->store_buffer, len, &written) == FR_OK &&
            written == len) ? 0 : -1;
}

static int ftp_close_file_objects(ftp_session_t *s)
{
    int failed = 0;

    if (s->file_open != 0U) {
        if (s->transfer == FTP_TRANSFER_STOR && ftp_store_flush(s) != 0) {
            failed = 1;
        }
        if (s->transfer == FTP_TRANSFER_STOR &&
            f_sync(&s->file) != FR_OK) {
            failed = 1;
        }
        if (f_close(&s->file) != FR_OK) {
            failed = 1;
        }
        s->file_open = 0U;
    }
    if (s->dir_open != 0U) {
        if (f_closedir(&s->dir) != FR_OK) {
            failed = 1;
        }
        s->dir_open = 0U;
    }
    return failed != 0 ? -1 : 0;
}

static uint8_t ftp_reset_transfer_state(ftp_session_t *s)
{
    const uint8_t changed = s->write_changed;

    s->data_socket = FTP_NO_SOCKET;
    s->data_listening = 0U;
    s->data_connecting = 0U;
    s->data_connected = 0U;
    s->data_tx_pending = 0U;
    s->data_buffer_len = 0U;
    s->store_len = 0U;
    s->send_pos = 0U;
    s->passive_wait = 0U;
    s->transfer = FTP_TRANSFER_NONE;
    s->write_changed = 0U;
    s->pending_stor_path[0] = '\0';
    return changed;
}

static void ftp_note_transfer_write(uint8_t changed)
{
    if (changed != 0U) {
        screenshot_service_note_local_sd_write_complete();
    }
}

static void ftp_abort_transfer(ftp_session_t *s)
{
    uint8_t changed;

    (void)ftp_close_file_objects(s);
    if (s->data_socket != FTP_NO_SOCKET) {
        socket_close(s->data_socket);
    }
    changed = ftp_reset_transfer_state(s);
    ftp_note_transfer_write(changed);
}

static void ftp_finish_transfer(ftp_session_t *s, uint8_t success)
{
    uint8_t changed;

    if (ftp_close_file_objects(s) != 0) {
        success = 0U;
    }
    socket_release(s->data_socket);
    changed = ftp_reset_transfer_state(s);
    ftp_note_transfer_write(changed);
    (void)ftp_reply(s, success != 0U ?
                    "226 Transfer complete.\r\n" :
                    "426 Transfer aborted.\r\n");
}

static int normalize_virtual_path(const char *cwd,
                                  const char *input,
                                  char output[FTP_PATH_LEN])
{
    size_t len;
    const char *p;

    if (output == NULL) {
        return -1;
    }
    if (input == NULL || input[0] == '\0') {
        (void)snprintf(output, FTP_PATH_LEN, "%s", cwd);
        return 0;
    }
    if (input[0] == '/' || input[0] == '\\') {
        output[0] = '/';
        output[1] = '\0';
        len = 1U;
    } else {
        len = strlen(cwd);
        if (len >= FTP_PATH_LEN) {
            return -1;
        }
        memcpy(output, cwd, len + 1U);
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

static int make_fat_path(const ftp_session_t *s,
                         const char *input,
                         char virtual_path[FTP_PATH_LEN],
                         char fat_path[FTP_FAT_PATH_LEN])
{
    int len;

    if (normalize_virtual_path(s->cwd, input, virtual_path) != 0 ||
        http_seed_path_private(virtual_path)) {
        return -1;
    }
    len = snprintf(fat_path, FTP_FAT_PATH_LEN, "0:%s", virtual_path);
    if (len < 0 || (size_t)len >= FTP_FAT_PATH_LEN) {
        return -1;
    }
    return 0;
}

static uint8_t data_socket_available(const ftp_session_t *s)
{
    uint8_t status;

    if (s->data_socket == FTP_NO_SOCKET ||
        uthernet2_w5100_socket_status(s->data_socket, &status) != 0) {
        return 0U;
    }
    return (uint8_t)(status == W5100_SR_LISTEN ||
                     status == W5100_SR_ESTABLISHED);
}

static int ftp_open_store_file(ftp_session_t *s)
{
    BYTE mode = FA_CREATE_ALWAYS | FA_WRITE;

    if (s->file_open != 0U) {
        return 0;
    }
    if (s->stor_append != 0U) {
        mode = FA_OPEN_APPEND | FA_WRITE;
    } else if (s->stor_offset != 0U) {
        mode = FA_OPEN_ALWAYS | FA_WRITE;
    }
    if (s->pending_stor_path[0] == '\0' ||
        f_open(&s->file, s->pending_stor_path, mode) != FR_OK) {
        return -1;
    }
    s->file_open = 1U;
    s->write_changed = 1U;
    if (s->stor_append == 0U && s->stor_offset != 0U &&
        (f_lseek(&s->file, s->stor_offset) != FR_OK ||
         f_truncate(&s->file) != FR_OK)) {
        return -1;
    }
    return 0;
}

/* Returns 0 when the data socket listens, 1 when no socket is free yet
 * and -1 on a chip error. */
static int ftp_open_passive(ftp_session_t *s)
{
    const uint8_t socket = data_socket_alloc();
    const uint16_t port =
        (uint16_t)(FTP_PASSIVE_PORT + g_ftp.next_passive);

    if (socket == FTP_NO_SOCKET) {
        return 1;
    }
    g_ftp.next_passive =
        (uint16_t)((g_ftp.next_passive + 1U) % FTP_PASSIVE_PORT_COUNT);
    if (socket_listen(socket, port) != 0) {
        return -1;
    }
    s->data_socket = socket;
    s->data_port = port;
    s->data_listening = 1U;
    return 0;
}

static void ftp_passive_reply(ftp_session_t *s)
{
    if (s->passive_wait == 2U) {
        (void)ftp_reply(s, "229 Entering Extended Passive Mode (|||%u|).\r\n",
                        (unsigned)s->data_port);
    } else {
        (void)ftp_reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
                        (unsigned)g_ftp.network.ip[0],
                        (unsigned)g_ftp.network.ip[1],
                        (unsigned)g_ftp.network.ip[2],
                        (unsigned)g_ftp.network.ip[3],
                        (unsigned)(s->data_port >> 8U),
                        (unsigned)(s->data_port & 0xFFU));
    }
    s->passive_wait = 0U;
}

/* PASV and EPSV. With three clients there is one data socket: the reply
 * waits until that socket is free. A note line each FTP_WAIT_NOTE_MS
 * keeps the client from a timeout. kind: 1 = PASV, 2 = EPSV. */
static void ftp_passive_poll(ftp_session_t *s)
{
    const int opened = ftp_open_passive(s);

    if (opened == 0) {
        ftp_passive_reply(s);
    } else if (opened < 0) {
        s->passive_wait = 0U;
        (void)ftp_reply(s, "425 Cannot open passive socket.\r\n");
    } else if (s->control_out_len == 0U && s->control_tx_pending == 0U &&
               ftp_now_ms() - s->wait_note_ms >= FTP_WAIT_NOTE_MS) {
        s->wait_note_ms = ftp_now_ms();
        (void)ftp_reply(s, "%s-Waiting for the data socket.\r\n",
                        s->passive_wait == 2U ? "229" : "227");
    }
}

/* Active mode: connects from port 20 to the address that the client gave
 * with PORT or EPRT. */
static int ftp_open_active(ftp_session_t *s)
{
    const uint8_t socket = data_socket_alloc();
    uint8_t status;

    if (socket == FTP_NO_SOCKET) {
        return -1;
    }
    socket_close(socket);
    if (uthernet2_write_reg(
            uthernet2_w5100_socket_reg(socket, W5100_SN_MR),
            W5100_SN_MR_TCP) != 0 ||
        uthernet2_w5100_write16(
            uthernet2_w5100_socket_reg(socket, W5100_SN_PORT),
            FTP_ACTIVE_PORT) != 0 ||
        uthernet2_w5100_socket_command(socket, W5100_CR_OPEN) != 0 ||
        uthernet2_w5100_socket_status(socket, &status) != 0 ||
        status != W5100_SR_INIT ||
        uthernet2_write_block(
            uthernet2_w5100_socket_reg(socket, W5100_SN_DIPR),
            s->control_peer, UTHERNET2_IPV4_LEN) != 0 ||
        uthernet2_w5100_write16(
            uthernet2_w5100_socket_reg(socket, FTP_SN_DPORT),
            s->active_port) != 0 ||
        uthernet2_w5100_socket_command(socket, FTP_CR_CONNECT) != 0) {
        socket_close(socket);
        return -1;
    }
    s->data_socket = socket;
    s->data_connecting = 1U;
    return 0;
}

static int ftp_prepare_transfer(ftp_session_t *s,
                                ftp_transfer_t transfer,
                                const char *argument,
                                uint32_t restart_offset,
                                uint8_t append)
{
    char virtual_path[FTP_PATH_LEN];
    char fat_path[FTP_FAT_PATH_LEN];
    FRESULT fr;

    if (s->transfer != FTP_TRANSFER_NONE) {
        (void)ftp_reply(s, "425 A transfer is in progress.\r\n");
        return -1;
    }
    if (s->active_mode == 0U && data_socket_available(s) == 0U) {
        (void)ftp_reply(s, "425 Use PASV, EPSV or PORT first.\r\n");
        return -1;
    }
    if (make_fat_path(s, argument, virtual_path, fat_path) != 0) {
        (void)ftp_reply(s, "550 Invalid path.\r\n");
        return -1;
    }
    if (transfer == FTP_TRANSFER_LIST || transfer == FTP_TRANSFER_NLST) {
        fr = f_opendir(&s->dir, fat_path);
        if (fr != FR_OK) {
            (void)ftp_reply(s, "550 Cannot open directory.\r\n");
            return -1;
        }
        s->dir_open = 1U;
    } else if (transfer == FTP_TRANSFER_RETR) {
        fr = f_open(&s->file, fat_path, FA_READ);
        if (fr != FR_OK) {
            (void)ftp_reply(s, "550 Cannot open file.\r\n");
            return -1;
        }
        s->file_open = 1U;
        if (restart_offset != 0U &&
            f_lseek(&s->file, restart_offset) != FR_OK) {
            (void)ftp_close_file_objects(s);
            (void)ftp_reply(s, "550 Cannot restart at that offset.\r\n");
            return -1;
        }
    } else if (transfer == FTP_TRANSFER_STOR) {
        (void)snprintf(s->pending_stor_path,
                       sizeof(s->pending_stor_path),
                       "%s",
                       fat_path);
        s->stor_offset = restart_offset;
        s->stor_append = append;
    }
    if (s->active_mode != 0U) {
        s->active_mode = 0U;
        if (ftp_open_active(s) != 0) {
            (void)ftp_close_file_objects(s);
            s->pending_stor_path[0] = '\0';
            (void)ftp_reply(s, "425 No data socket is free. Try again.\r\n");
            return -1;
        }
    }
    s->transfer = transfer;
    if (transfer == FTP_TRANSFER_STOR && s->data_connected != 0U &&
        ftp_open_store_file(s) != 0) {
        ftp_abort_transfer(s);
        (void)ftp_reply(s, "550 Cannot create file.\r\n");
        return -1;
    }
    (void)ftp_reply(s, "150 Opening data connection.\r\n");
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

/* PORT h1,h2,h3,h4,p1,p2 and EPRT |1|address|port|. The address must be
 * that of the control connection: the server does not connect to a third
 * machine. */
static void ftp_handle_port(ftp_session_t *s, const char *argument,
                            uint8_t extended)
{
    unsigned v[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    unsigned port;
    int fields;

    if (extended != 0U) {
        fields = argument[0] == '\0' ? 0 :
            sscanf(&argument[1], "1%*c%u.%u.%u.%u%*c%u",
                   &v[0], &v[1], &v[2], &v[3], &v[4]);
        port = v[4];
        if (fields != 5) {
            (void)ftp_reply(s, "522 Use network protocol (1).\r\n");
            return;
        }
    } else {
        fields = sscanf(argument, "%u,%u,%u,%u,%u,%u",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        port = (v[4] << 8U) | v[5];
        if (fields != 6 || v[4] > 255U || v[5] > 255U) {
            (void)ftp_reply(s, "501 Bad PORT argument.\r\n");
            return;
        }
    }
    if (port == 0U || port > 65535U ||
        v[0] != s->control_peer[0] || v[1] != s->control_peer[1] ||
        v[2] != s->control_peer[2] || v[3] != s->control_peer[3]) {
        (void)ftp_reply(s, "501 The address must be that of the client.\r\n");
        return;
    }
    ftp_abort_transfer(s);
    s->active_port = (uint16_t)port;
    s->active_mode = 1U;
    (void)ftp_reply(s, "200 Active mode address accepted.\r\n");
}

static void ftp_handle_command(ftp_session_t *s, char *line)
{
    char *argument;
    char virtual_path[FTP_PATH_LEN];
    char fat_path[FTP_FAT_PATH_LEN];
    FILINFO info;
    FRESULT fr;
    const uint32_t restart_offset = s->restart_offset;

    s->restart_offset = 0U;
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
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "USER") == 0) {
        s->logged_in = 0U;
        (void)ftp_reply(s, "331 Anonymous login; any password is accepted.\r\n");
        return;
    }
    if (strcmp(line, "PASS") == 0) {
        s->logged_in = 1U;
        (void)ftp_reply(s, "230 Anonymous read/write access granted.\r\n");
        return;
    }
    if (strcmp(line, "QUIT") == 0) {
        ftp_abort_transfer(s);
        (void)ftp_reply(s, "221 Goodbye.\r\n");
        s->close_control_after_tx = 1U;
        return;
    }
    /* Clients send these before the login. */
    if (strcmp(line, "AUTH") == 0) {
        (void)ftp_reply(s, "534 TLS is not available.\r\n");
        return;
    }
    if (strcmp(line, "SYST") == 0) {
        (void)ftp_reply(s, "215 UNIX Type: L8\r\n");
        return;
    }
    if (strcmp(line, "FEAT") == 0) {
        (void)ftp_reply(s, "211-Features:\r\n EPRT\r\n EPSV\r\n MDTM\r\n PASV\r\n"
                        " REST STREAM\r\n SIZE\r\n211 End\r\n");
        return;
    }
    if (strcmp(line, "OPTS") == 0) {
        (void)ftp_reply(s, "200 Option accepted.\r\n");
        return;
    }
    if (strcmp(line, "NOOP") == 0) {
        (void)ftp_reply(s, "200 NOOP ok.\r\n");
        return;
    }
    if (s->logged_in == 0U) {
        (void)ftp_reply(s, "530 Log in with USER and PASS first.\r\n");
        return;
    }

    if (strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 ||
        strcmp(line, "STRU") == 0) {
        (void)ftp_reply(s, "200 Transfer setting accepted.\r\n");
    } else if (strcmp(line, "ALLO") == 0) {
        (void)ftp_reply(s, "202 ALLO is not necessary.\r\n");
    } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
        (void)ftp_reply(s, "257 \"%s\" is the current directory.\r\n", s->cwd);
    } else if (strcmp(line, "CWD") == 0 || strcmp(line, "XCWD") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            (strcmp(virtual_path, "/") != 0 &&
             (f_stat(fat_path, &info) != FR_OK ||
              (info.fattrib & AM_DIR) == 0U))) {
            (void)ftp_reply(s, "550 Directory unavailable.\r\n");
        } else {
            (void)snprintf(s->cwd, sizeof(s->cwd), "%s", virtual_path);
            (void)ftp_reply(s, "250 Directory changed.\r\n");
        }
    } else if (strcmp(line, "CDUP") == 0 || strcmp(line, "XCUP") == 0) {
        if (normalize_virtual_path(s->cwd, "..", virtual_path) == 0) {
            (void)snprintf(s->cwd, sizeof(s->cwd), "%s", virtual_path);
        }
        (void)ftp_reply(s, "250 Directory changed.\r\n");
    } else if (strcmp(line, "PASV") == 0 || strcmp(line, "EPSV") == 0) {
        if (line[0] == 'E' && (strcmp(argument, "ALL") == 0 ||
                               strcmp(argument, "all") == 0)) {
            (void)ftp_reply(s, "200 EPSV ALL accepted.\r\n");
        } else {
            ftp_abort_transfer(s);
            s->active_mode = 0U;
            s->passive_wait = line[0] == 'E' ? 2U : 1U;
            s->wait_note_ms = ftp_now_ms();
            ftp_passive_poll(s);
        }
    } else if (strcmp(line, "PORT") == 0) {
        ftp_handle_port(s, argument, 0U);
    } else if (strcmp(line, "EPRT") == 0) {
        ftp_handle_port(s, argument, 1U);
    } else if (strcmp(line, "REST") == 0) {
        unsigned long offset = 0UL;

        if (sscanf(argument, "%lu", &offset) != 1) {
            (void)ftp_reply(s, "501 Bad REST argument.\r\n");
        } else {
            s->restart_offset = (uint32_t)offset;
            (void)ftp_reply(s, "350 Restart position accepted.\r\n");
        }
    } else if (strcmp(line, "LIST") == 0 || strcmp(line, "NLST") == 0) {
        /* Clients send options such as -a or -la before the path. */
        while (argument[0] == '-') {
            while (*argument != '\0' && *argument != ' ') {
                ++argument;
            }
            while (*argument == ' ') {
                ++argument;
            }
        }
        (void)ftp_prepare_transfer(s,
                                   line[0] == 'L' ? FTP_TRANSFER_LIST :
                                                    FTP_TRANSFER_NLST,
                                   argument, 0U, 0U);
    } else if (strcmp(line, "RETR") == 0) {
        (void)ftp_prepare_transfer(s, FTP_TRANSFER_RETR, argument,
                                   restart_offset, 0U);
    } else if (strcmp(line, "STOR") == 0) {
        (void)ftp_prepare_transfer(s, FTP_TRANSFER_STOR, argument,
                                   restart_offset, 0U);
    } else if (strcmp(line, "APPE") == 0) {
        (void)ftp_prepare_transfer(s, FTP_TRANSFER_STOR, argument, 0U, 1U);
    } else if (strcmp(line, "ABOR") == 0) {
        ftp_abort_transfer(s);
        (void)ftp_reply(s, "226 Abort complete.\r\n");
    } else if (strcmp(line, "SIZE") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK || (info.fattrib & AM_DIR) != 0U) {
            (void)ftp_reply(s, "550 File unavailable.\r\n");
        } else {
            (void)ftp_reply(s, "213 %llu\r\n",
                            (unsigned long long)info.fsize);
        }
    } else if (strcmp(line, "MDTM") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK) {
            (void)ftp_reply(s, "550 File unavailable.\r\n");
        } else {
            (void)ftp_reply(s, "213 %04u%02u%02u%02u%02u%02u\r\n",
                            (unsigned)(1980U + (info.fdate >> 9U)),
                            (unsigned)((info.fdate >> 5U) & 0x0FU),
                            (unsigned)(info.fdate & 0x1FU),
                            (unsigned)(info.ftime >> 11U),
                            (unsigned)((info.ftime >> 5U) & 0x3FU),
                            (unsigned)((info.ftime & 0x1FU) * 2U));
        }
    } else if (strcmp(line, "DELE") == 0 || strcmp(line, "RMD") == 0 ||
               strcmp(line, "XRMD") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            f_unlink(fat_path) != FR_OK) {
            (void)ftp_reply(s, "550 Delete failed.\r\n");
        } else {
            screenshot_service_note_local_sd_write_complete();
            (void)ftp_reply(s, "250 Deleted.\r\n");
        }
    } else if (strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            f_mkdir(fat_path) != FR_OK) {
            (void)ftp_reply(s, "550 Create directory failed.\r\n");
        } else {
            screenshot_service_note_local_sd_write_complete();
            (void)ftp_reply(s, "257 \"%s\" created.\r\n", virtual_path);
        }
    } else if (strcmp(line, "RNFR") == 0) {
        if (make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            f_stat(fat_path, &info) != FR_OK) {
            s->rename_from[0] = '\0';
            (void)ftp_reply(s, "550 Source unavailable.\r\n");
        } else {
            (void)snprintf(s->rename_from, sizeof(s->rename_from),
                           "%s", virtual_path);
            (void)ftp_reply(s, "350 Send RNTO.\r\n");
        }
    } else if (strcmp(line, "RNTO") == 0) {
        char old_fat[FTP_FAT_PATH_LEN];

        if (s->rename_from[0] == '\0' ||
            make_fat_path(s, argument, virtual_path, fat_path) != 0 ||
            snprintf(old_fat, sizeof(old_fat), "0:%s", s->rename_from) < 0) {
            (void)ftp_reply(s, "503 Send RNFR first.\r\n");
        } else {
            fr = f_rename(old_fat, fat_path);
            s->rename_from[0] = '\0';
            if (fr != FR_OK) {
                (void)ftp_reply(s, "550 Rename failed.\r\n");
            } else {
                screenshot_service_note_local_sd_write_complete();
                (void)ftp_reply(s, "250 Renamed.\r\n");
            }
        }
    } else if (strcmp(line, "STAT") == 0) {
        (void)ftp_reply(s, "211 Appletini ONE anonymous FTP ready.\r\n");
    } else {
        (void)ftp_reply(s, "502 Command not implemented.\r\n");
    }
}

/* Ends a session. graceful: the client gets a TCP close handshake. */
static void ftp_session_end(ftp_session_t *s, uint8_t graceful)
{
    ftp_abort_transfer(s);
    if (graceful != 0U) {
        socket_release(s->control_socket);
    } else {
        socket_close(s->control_socket);
    }
    s->used = 0U;
}

static void ftp_control_output_poll(ftp_session_t *s)
{
    int ready;

    if (s->used == 0U) {
        return;
    }
    ready = socket_send_ready(s->control_socket, &s->control_tx_pending);
    if (ready < 0) {
        ftp_session_end(s, 0U);
        return;
    }
    if (ready == 0) {
        return;
    }
    if (s->control_out_len != 0U) {
        const int sent = socket_send_buffer(
            s->control_socket,
            (const uint8_t *)s->control_out,
            &s->control_out_len,
            &s->control_tx_pending);
        if (sent < 0) {
            ftp_session_end(s, 0U);
        }
        return;
    }
    if (s->close_control_after_tx != 0U) {
        ftp_session_end(s, 1U);
    }
}

static void ftp_control_input_poll(ftp_session_t *s)
{
    uint16_t received;
    char *newline;
    size_t line_len;

    if (s->used == 0U || s->control_out_len != 0U ||
        s->control_tx_pending != 0U || s->close_control_after_tx != 0U ||
        s->passive_wait != 0U) {
        return;
    }
    if (s->control_rx_len < sizeof(s->control_rx) - 1U) {
        if (socket_receive(s->control_socket,
                           (uint8_t *)&s->control_rx[s->control_rx_len],
                           (uint16_t)(sizeof(s->control_rx) -
                                      s->control_rx_len - 1U),
                           &received) != 0) {
            ftp_session_end(s, 0U);
            return;
        }
        s->control_rx_len = (uint16_t)(s->control_rx_len + received);
        s->control_rx[s->control_rx_len] = '\0';
    }
    newline = strchr(s->control_rx, '\n');
    if (newline == NULL) {
        if (s->control_rx_len == sizeof(s->control_rx) - 1U) {
            s->control_rx_len = 0U;
            s->control_rx[0] = '\0';
            (void)ftp_reply(s, "500 Command too long.\r\n");
        }
        return;
    }
    line_len = (size_t)(newline - s->control_rx);
    if (line_len != 0U && s->control_rx[line_len - 1U] == '\r') {
        --line_len;
    }
    s->control_rx[line_len] = '\0';
    ftp_handle_command(s, s->control_rx);
    {
        const size_t consumed = (size_t)(newline - s->control_rx) + 1U;
        const size_t remaining = s->control_rx_len - consumed;

        memmove(s->control_rx, newline + 1, remaining);
        s->control_rx_len = (uint16_t)remaining;
        s->control_rx[remaining] = '\0';
    }
}

static void ftp_control_poll(ftp_session_t *s)
{
    uint8_t status;

    if (uthernet2_w5100_socket_status(s->control_socket, &status) != 0) {
        return;
    }
    if (status == W5100_SR_CLOSED) {
        ftp_session_end(s, 0U);
        return;
    }
    if (status == W5100_SR_CLOSE_WAIT) {
        ftp_session_end(s, 1U);
        return;
    }
    if (status != W5100_SR_ESTABLISHED) {
        return;
    }
    if (s->passive_wait != 0U) {
        ftp_passive_poll(s);
    }
    ftp_control_output_poll(s);
    ftp_control_input_poll(s);
    ftp_control_output_poll(s);
}

/* A client connected to the listening socket: that socket becomes the
 * control socket of a new session. */
static void ftp_session_begin(uint8_t socket)
{
    ftp_session_t *s = NULL;
    uint8_t peer[UTHERNET2_IPV4_LEN];

    for (uint8_t i = 0U; i < FTP_SESSION_COUNT; ++i) {
        if (g_ftp.sessions[i].used == 0U) {
            s = &g_ftp.sessions[i];
            break;
        }
    }
    if (s == NULL) {
        socket_close(socket);
        return;
    }
    memset(s, 0, sizeof(*s));
    s->used = 1U;
    s->control_socket = socket;
    s->data_socket = FTP_NO_SOCKET;
    (void)snprintf(s->cwd, sizeof(s->cwd), "/");
    if (socket_peer(socket, peer) != 0 ||
        peer_on_local_subnet(peer) == 0U) {
        (void)ftp_reply(s, "421 Appletini FTP accepts local-subnet clients only.\r\n");
        s->close_control_after_tx = 1U;
    } else {
        memcpy(s->control_peer, peer, sizeof(s->control_peer));
        (void)ftp_reply(s, "220 Appletini ONE anonymous FTP ready.\r\n");
    }
}

/* Keeps one socket listening on port 21 while a session and a socket are
 * free. The chip has no connection queue: a client that connects while no
 * socket listens is refused. */
static void ftp_listener_poll(void)
{
    uint8_t status;
    uint8_t sessions = 0U;

    if (g_ftp.listen_socket != FTP_NO_SOCKET &&
        uthernet2_w5100_socket_status(g_ftp.listen_socket, &status) == 0) {
        if (status == W5100_SR_ESTABLISHED ||
            status == W5100_SR_CLOSE_WAIT) {
            const uint8_t socket = g_ftp.listen_socket;

            g_ftp.listen_socket = FTP_NO_SOCKET;
            ftp_session_begin(socket);
        } else if (status == W5100_SR_CLOSED) {
            g_ftp.listen_socket = FTP_NO_SOCKET;
        }
    }
    if (g_ftp.listen_socket != FTP_NO_SOCKET) {
        return;
    }
    for (uint8_t i = 0U; i < FTP_SESSION_COUNT; ++i) {
        sessions = (uint8_t)(sessions + g_ftp.sessions[i].used);
    }
    if (sessions >= FTP_SESSION_COUNT) {
        return;
    }
    for (uint8_t socket = FTP_SOCKET_COUNT; socket-- != 0U;) {
        if (socket_free(socket) != 0U) {
            if (socket_listen(socket, FTP_CONTROL_PORT) == 0) {
                g_ftp.listen_socket = socket;
            }
            return;
        }
    }
}

/* Follows the data socket until the client is connected. Returns 1 when
 * the connection is ready for the transfer. */
static uint8_t ftp_data_connect_poll(ftp_session_t *s, uint8_t status)
{
    uint8_t peer[UTHERNET2_IPV4_LEN];

    if (s->data_connected != 0U) {
        return 1U;
    }
    if (s->data_connecting != 0U) {
        if (status == W5100_SR_CLOSED) {
            ftp_abort_transfer(s);
            (void)ftp_reply(s, "425 Cannot open the data connection.\r\n");
            return 0U;
        }
        if (status != W5100_SR_ESTABLISHED) {
            return 0U;
        }
    } else if (s->data_listening != 0U) {
        if (status != W5100_SR_ESTABLISHED &&
            status != W5100_SR_CLOSE_WAIT) {
            return 0U;
        }
        if (socket_peer(s->data_socket, peer) != 0 ||
            peer_on_local_subnet(peer) == 0U ||
            memcmp(peer, s->control_peer, sizeof(peer)) != 0) {
            /* Reject only the rogue socket. Keep the command, open file or
             * directory, and passive transfer ready for the control client. */
            if (socket_listen(s->data_socket, s->data_port) != 0) {
                ftp_abort_transfer(s);
                (void)ftp_reply(s, "425 Cannot reopen passive data socket.\r\n");
            }
            return 0U;
        }
    } else {
        return 0U;
    }
    if (s->transfer == FTP_TRANSFER_STOR &&
        ftp_open_store_file(s) != 0) {
        ftp_abort_transfer(s);
        (void)ftp_reply(s, "550 Cannot create file.\r\n");
        return 0U;
    }
    s->data_connected = 1U;
    s->data_listening = 0U;
    s->data_connecting = 0U;
    return 1U;
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
                    "%s 1 ftp ftp %10llu %s %02u %02u:%02u %s\r\n",
                    (info->fattrib & AM_DIR) != 0U ?
                        "drwxrwxrwx" : "-rw-rw-rw-",
                    (unsigned long long)info->fsize,
                    (month >= 1U && month <= 12U) ? months[month - 1U] : "Jan",
                    day,
                    hour,
                    minute,
                    info->fname);
}

/* The number of bytes for one SEND command. One SEND must fit in one TCP
 * segment (1460 bytes on Ethernet): with more, the chip waits for the
 * delayed ACK of the client and a download drops to about 100 kB/s
 * (measured with 4 KiB in F1.3.9dev20). Sockets 2 and 3 have a 1 KiB
 * transmit buffer. */
static uint32_t socket_tx_size(uint8_t socket)
{
    return socket <= 1U ? 1460U : 1024U;
}

static void ftp_send_data_buffer(ftp_session_t *s)
{
    if (socket_send_buffer(s->data_socket,
                           s->data_buffer,
                           &s->data_buffer_len,
                           &s->data_tx_pending) < 0) {
        ftp_finish_transfer(s, 0U);
    }
}

static void ftp_send_transfer_poll(ftp_session_t *s)
{
    int ready;
    UINT bytes_read;

    ready = socket_send_ready(s->data_socket, &s->data_tx_pending);
    if (ready < 0) {
        ftp_finish_transfer(s, 0U);
        return;
    }
    if (ready == 0) {
        return;
    }
    if (s->data_buffer_len != 0U) {
        ftp_send_data_buffer(s);
        return;
    }

    if (s->transfer == FTP_TRANSFER_RETR) {
        uint32_t chunk;
        int sent;

        /* Reads 32 KiB from the SD card at a time and sends one TCP
         * segment for each SEND command. */
        if (s->send_pos == s->store_len) {
            if (f_read(&s->file,
                       s->store_buffer,
                       sizeof(s->store_buffer),
                       &bytes_read) != FR_OK) {
                ftp_finish_transfer(s, 0U);
                return;
            }
            if (bytes_read == 0U) {
                ftp_finish_transfer(s, 1U);
                return;
            }
            s->store_len = (uint32_t)bytes_read;
            s->send_pos = 0U;
        }
        chunk = s->store_len - s->send_pos;
        if (chunk > socket_tx_size(s->data_socket)) {
            chunk = socket_tx_size(s->data_socket);
        }
        sent = socket_send_start(s->data_socket,
                                 &s->store_buffer[s->send_pos],
                                 (uint16_t)chunk,
                                 &s->data_tx_pending);
        if (sent < 0) {
            ftp_finish_transfer(s, 0U);
        } else if (sent > 0) {
            s->send_pos += chunk;
        }
        return;
    }

    if (s->transfer == FTP_TRANSFER_LIST ||
        s->transfer == FTP_TRANSFER_NLST) {
        FILINFO info;
        int len;

        if (f_readdir(&s->dir, &info) != FR_OK) {
            ftp_finish_transfer(s, 0U);
            return;
        }
        if (info.fname[0] == '\0') {
            ftp_finish_transfer(s, 1U);
            return;
        }
        len = ftp_format_list_line(&info,
                                   (uint8_t)(s->transfer == FTP_TRANSFER_NLST),
                                   s->data_buffer,
                                   sizeof(s->data_buffer));
        if (len <= 0 || (size_t)len >= sizeof(s->data_buffer)) {
            ftp_finish_transfer(s, 0U);
            return;
        }
        s->data_buffer_len = (uint16_t)len;
        ftp_send_data_buffer(s);
    }
}

static void ftp_store_transfer_poll(ftp_session_t *s)
{
    uint8_t status;
    uint16_t received;
    uint32_t room = (uint32_t)sizeof(s->store_buffer) - s->store_len;

    /* Takes all bytes that the chip holds, direct into the write buffer. */
    if (room > FTP_STORE_RECEIVE_MAX) {
        room = FTP_STORE_RECEIVE_MAX;
    }
    if (socket_receive(s->data_socket,
                       &s->store_buffer[s->store_len],
                       (uint16_t)room,
                       &received) != 0) {
        ftp_finish_transfer(s, 0U);
        return;
    }
    if (received != 0U) {
        s->store_len += received;
        if (s->store_len == sizeof(s->store_buffer) &&
            ftp_store_flush(s) != 0) {
            ftp_finish_transfer(s, 0U);
        }
        return;
    }
    if (uthernet2_w5100_socket_status(s->data_socket, &status) != 0) {
        ftp_finish_transfer(s, 0U);
    } else if (status == W5100_SR_CLOSE_WAIT || status == W5100_SR_CLOSED) {
        ftp_finish_transfer(s, 1U);
    }
}

static void ftp_data_poll(ftp_session_t *s)
{
    uint8_t status;

    if (s->used == 0U || s->data_socket == FTP_NO_SOCKET ||
        uthernet2_w5100_socket_status(s->data_socket, &status) != 0 ||
        ftp_data_connect_poll(s, status) == 0U ||
        s->transfer == FTP_TRANSFER_NONE) {
        return;
    }
    if (status != W5100_SR_ESTABLISHED && status != W5100_SR_CLOSE_WAIT) {
        ftp_finish_transfer(s, 0U);
        return;
    }
    if (s->transfer == FTP_TRANSFER_STOR) {
        ftp_store_transfer_poll(s);
    } else {
        /* A client may half-close its write side after issuing RETR or LIST.
         * W5100S still permits server-to-client sends in CLOSE_WAIT, so keep
         * draining the transfer until EOF and the final SEND completes. */
        ftp_send_transfer_poll(s);
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
    g_ftp.listen_socket = FTP_NO_SOCKET;
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
    for (uint8_t socket = 0U; socket < FTP_SOCKET_COUNT; ++socket) {
        socket_close(socket);
    }
    /* The control connections take the small sockets (3, 2, 1) first; the
     * data connections take the large ones (0, 1) first. */
    if (uthernet2_write_reg(W5100_REG_RMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        uthernet2_write_reg(W5100_REG_TMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        socket_listen(FTP_SOCKET_COUNT - 1U, FTP_CONTROL_PORT) != 0) {
        (void)f_mount((FATFS *)0, "0:/", 0U);
        set_detail(detail, detail_len, "FTP COULD NOT OPEN TCP PORT 21");
        return -1;
    }
    g_ftp.listen_socket = FTP_SOCKET_COUNT - 1U;
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
    for (uint8_t i = 0U; i < FTP_SESSION_COUNT; ++i) {
        if (g_ftp.sessions[i].used != 0U) {
            ftp_abort_transfer(&g_ftp.sessions[i]);
        }
    }
    for (uint8_t socket = 0U; socket < FTP_SOCKET_COUNT; ++socket) {
        socket_close(socket);
    }
    (void)f_mount((FATFS *)0, "0:/", 0U);
    memset(&g_ftp, 0, sizeof(g_ftp));
}

void ftp_sd_service_poll(void)
{
    if (g_ftp.active == 0U) {
        return;
    }
    ftp_listener_poll();
    for (uint8_t i = 0U; i < FTP_SESSION_COUNT; ++i) {
        ftp_session_t *s = &g_ftp.sessions[i];

        if (s->used == 0U) {
            continue;
        }
        ftp_control_poll(s);
        ftp_data_poll(s);
        ftp_control_output_poll(s);
    }
}

uint8_t ftp_sd_service_active(void)
{
    return g_ftp.active;
}
