/* Appletini web server for an NMOS 6502. */

#include <conio.h>
#include <stdint.h>
#include <string.h>

#include "appletini_net.h"
#include "ip65_min.h"

#define RESPONSE_CAP   640U
static char response[RESPONSE_CAP];
static uint16_t response_len;

static void append_char(char value)
{
    if (response_len < RESPONSE_CAP) {
        response[response_len++] = value;
    }
}

static void append_text(const char *text)
{
    while (*text != '\0') {
        append_char(*text++);
    }
}

static void append_u8(uint8_t value)
{
    if (value >= 100U) {
        append_char((char)('0' + value / 100U));
        value %= 100U;
        append_char((char)('0' + value / 10U));
    } else if (value >= 10U) {
        append_char((char)('0' + value / 10U));
    }
    append_char((char)('0' + value % 10U));
}

static void append_ip(const uint8_t *ip)
{
    uint8_t i;

    for (i = 0U; i < 4U; ++i) {
        if (i != 0U) {
            append_char('.');
        }
        append_u8(ip[i]);
    }
}

static void print_u8(uint8_t value)
{
    if (value >= 100U) {
        cputc((char)('0' + value / 100U));
        cputc((char)('0' + (value / 10U) % 10U));
    } else if (value >= 10U) {
        cputc((char)('0' + value / 10U));
    }
    cputc((char)('0' + value % 10U));
}

static void print_ip(const uint8_t *ip)
{
    uint8_t i;

    for (i = 0U; i < 4U; ++i) {
        if (i != 0U) {
            cputc('.');
        }
        print_u8(ip[i]);
    }
}

static uint8_t request_is_index(const char *method, const char *path)
{
    return (method[0] == 'G' || method[0] == 'g') &&
           (method[1] == 'E' || method[1] == 'e') &&
           (method[2] == 'T' || method[2] == 't') && method[3] == '\0' &&
           (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0);
}

static void build_page(void)
{
    response_len = 0U;
    append_text("<!doctype html><html><head><title>Appletini</title></head>"
                "<body><h1>Appletini Web Server</h1><p>Served by an Apple II "
                "running a 6502 TCP/IP stack.</p><table><tr><td>IP</td><td>");
    append_ip(appletini_config.ip);
    append_text("</td></tr><tr><td>Subnet</td><td>");
    append_ip(appletini_config.subnet);
    append_text("</td></tr><tr><td>Gateway</td><td>");
    append_ip(appletini_config.gateway);
    append_text("</td></tr></table></body></html>");
}

static void __fastcall__ http_server(uint32_t client,
                                     const char *method,
                                     const char *path)
{
    (void)client;
    if (request_is_index(method, path) == 0U) {
        httpd_send_response(HTTPD_RESPONSE_404, NULL, 0U);
        return;
    }
    build_page();
    httpd_send_response(HTTPD_RESPONSE_200_HTML,
                        (const uint8_t *)response,
                        response_len);
    cputc('.');
}

static void wait_for_key(void)
{
    cputs("\r\nPRESS ANY KEY\r\n");
    (void)cgetc();
}

int main(void)
{
    uint8_t status;

    clrscr();
    cputs("APPLETINI WEB SERVER\r\n\r\n");
    status = appletini_network_init();
    if (status != APPLETINI_NET_OK) {
        cputs(appletini_network_error(status));
        cputs("\r\n");
        wait_for_key();
        return 1;
    }

    cputs("URL: HTTP://");
    print_ip(appletini_config.ip);
    cputs("/\r\nSUBNET: ");
    print_ip(appletini_config.subnet);
    cputs("\r\nGATEWAY: ");
    print_ip(appletini_config.gateway);
    cputs("\r\nPRESS ESC TO STOP\r\n\r\n");

    httpd_start(80U, http_server);
    cputs("\r\nSERVER STOPPED\r\n");
    wait_for_key();
    return 0;
}
